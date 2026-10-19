#include "viewer/viewer_view.h"

#include <imgui.h>

#include <cfloat>
#include <format>

namespace fillscope {
namespace {

constexpr float kPlotHeight = 120.0f;
constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};
constexpr ImVec4 kFollowedRowColor{0.45f, 0.38f, 0.10f, 0.65f};

// Draws text that is already formatted. TextUnformatted takes the characters as they are, with no
// printf-style % processing, so a '%' in the text can't be misread as a format directive.
void text(std::string_view s) { ImGui::TextUnformatted(s.data(), s.data() + s.size()); }

// A tooltip for the item just drawn, shown while it is hovered.
void tooltip(const std::string& what) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", what.c_str());
    }
}

// The context bar: what the assistant has been given, drawn from the last published context and
// never from the live selection, so the bar and the file can't disagree.
void draw_context_bar(const std::optional<Context>& published, const std::optional<std::string>& error,
                      ViewerActions& actions) {
    ImGui::TextUnformatted("Context");
    ImGui::SameLine();
    if (!published) {
        text("nothing published yet");
    } else {
        const ContextBarText bar = bar_text(*published);
        text(bar.background);
        ImGui::Indent();
        if (bar.point) {
            text(*bar.point);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearPointButton)) {
                actions.clear_point = true;
            }
            tooltip("Stop pointing at it: sends that to the assistant now");
        }
        if (bar.follow) {
            text(*bar.follow);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearFollowButton)) {
                actions.clear_follow = true;
            }
            tooltip("Stop following it: sends that to the assistant now");
        }
        ImGui::Unindent();
    }
    if (error) {
        // Push and pop must pair; the style stack is checked at the end of the frame.
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        text(std::format("NOT published: {}", *error));
        ImGui::PopStyleColor();
    }
}

// The source's state, the latest frame and the viewer's own liveness.
void draw_status(const ViewerState& state) {
    text(std::format("source: {}", state.source_connected ? "connected" : "waiting"));
    const std::size_t received = state.run ? state.run->frames().size() : 0;
    text(std::format("frames received: {}", received));
    if (received > 0) {
        const Frame& last = state.run->frames().back();
        text(std::format("last frame: #{}  id {}  price {}  qty {}", last.index, last.id, last.price, last.qty));
    }
    text(std::format("seconds since last frame: {:.1f}", state.seconds_since_frame));
    text(std::format("viewer paints: {} (keeps rising while the source is stopped)", state.paints));
}

// The frames, one row each. Clicking a row points at its frame; clicking its id follows the id.
// Rows with the followed id are highlighted.
void draw_frame_table(const Run& run, const Selection& selection, ViewerActions& actions) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    // The table takes the rest of the window's height and scrolls within it.
    if (!ImGui::BeginTable(kFrameTable, 4, flags, ImVec2{0.0f, ImGui::GetContentRegionAvail().y})) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);  // the header row stays visible while scrolling
    ImGui::TableSetupColumn("frame");
    ImGui::TableSetupColumn("id");
    ImGui::TableSetupColumn("price");
    ImGui::TableSetupColumn("qty");
    ImGui::TableHeadersRow();

    const std::span<const Frame> frames = run.frames();
    // The clipper asks for only the rows that are visible, so a run of a million frames costs no
    // more per paint than one of fifty.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(frames.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const Frame& frame = frames[static_cast<std::size_t>(row)];
            ImGui::TableNextRow();
            if (selection.followed() == frame.id) {  // an empty optional compares unequal to any value
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(kFollowedRowColor));
            }

            ImGui::TableNextColumn();
            const bool pointed = selection.pointed() && selection.pointed()->index == frame.index;
            // SpanAllColumns makes the whole row the click target; AllowOverlap lets the id's
            // button, drawn on top of it, take its own clicks.
            if (ImGui::Selectable(point_label(frame).c_str(), pointed,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                actions.point_at = frame;
            }
            tooltip(std::format("Point at frame #{}: sends it to the assistant now", frame.index));

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(follow_label(frame).c_str())) {
                actions.follow = frame.id;
            }
            tooltip(std::format("Follow id {}: sends it to the assistant now, and highlights every frame with it",
                                frame.id));

            ImGui::TableNextColumn();
            text(std::format("{}", frame.price));
            ImGui::TableNextColumn();
            text(std::format("{}", frame.qty));
        }
    }
    ImGui::EndTable();
}

}  // namespace

void apply(const ViewerActions& actions, Selection& selection) {
    if (actions.point_at) {
        selection.point_at(*actions.point_at);
    }
    if (actions.follow) {
        selection.follow(*actions.follow);
    }
    if (actions.clear_point) {
        selection.clear_point();
    }
    if (actions.clear_follow) {
        selection.clear_follow();
    }
}

std::string point_label(const Frame& frame) { return std::format("{}##point", frame.index); }

std::string follow_label(const Frame& frame) { return std::format("{}##follow{}", frame.id, frame.index); }

ViewerActions draw_viewer(const ViewerState& state, const Selection& selection,
                          const std::optional<Context>& published, const std::optional<std::string>& publish_error) {
    ViewerActions actions;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(kViewerWindow, nullptr, ImGuiWindowFlags_NoDecoration);

    draw_context_bar(published, publish_error, actions);
    ImGui::Separator();
    draw_status(state);
    if (state.run && !state.run->prices().empty()) {
        const std::span<const float> prices = state.run->prices();
        ImGui::PlotLines(kPricePlot, prices.data(), static_cast<int>(prices.size()), 0, nullptr, FLT_MAX, FLT_MAX,
                         ImVec2{-1.0f, kPlotHeight});
        draw_frame_table(*state.run, selection, actions);
    }

    ImGui::End();
    return actions;
}

}  // namespace fillscope
