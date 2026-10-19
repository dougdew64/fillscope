#include "viewer/viewer_view.h"

#include <imgui.h>
#include <imgui_internal.h>  // IMGUI_TEST_ENGINE_ITEM_INFO, ImGuiItemStatusFlags

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fillscope {
namespace {

constexpr float kPlotHeight = 120.0f;
constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};
constexpr ImVec4 kFollowedRowColor{0.45f, 0.38f, 0.10f, 0.65f};

// Draws text that is already formatted. TextUnformatted takes the characters as they are, with no
// printf-style % processing, so a '%' in the text can't be misread as a format directive.
void text(std::string_view s) { ImGui::TextUnformatted(s.data(), s.data() + s.size()); }

// A tooltip for the item just drawn, while it is hovered. `describe` builds the text, and is called
// only while hovered, so no tooltip text is built for the rows nobody is pointing at.
// std::invocable<F> is a C++20 concept: it accepts anything callable with no arguments.
template <std::invocable F>
void tooltip(F&& describe) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        const std::string what = describe();
        ImGui::SetTooltip("%s", what.c_str());
    }
}

// Tells the Dear ImGui Test Engine, when one is built in, whether the item just drawn is marked
// (the pointed row, a followed id), as Dear ImGui does for a checkbox's tick. Dear ImGui doesn't
// report a Selectable's selected state or a row's colour, so without this the UI tests could check
// only the rule (row_marks), not the table as drawn. In the viewer itself the macro is empty.
void report_marked([[maybe_unused]] const Label& label, [[maybe_unused]] bool marked) {
    ImGuiContext& g = *ImGui::GetCurrentContext();  // the macro refers to the context as `g`
    IMGUI_TEST_ENGINE_ITEM_INFO(ImGui::GetItemID(), label.c_str(),
                                ImGui::GetItemStatusFlags() | ImGuiItemStatusFlags_Checkable |
                                    (marked ? ImGuiItemStatusFlags_Checked : 0));
}

// What a click does, for a tooltip: sent at once, unless writes are failing.
std::string_view sends(const std::optional<std::string>& publish_error) {
    return publish_error ? "but publishing is failing (see the context bar), so the assistant won't receive it"
                         : "sends it to the assistant now";
}

// The context bar: what the assistant has been given, drawn from the last published context and
// never from the live selection, so the bar and the file can't disagree.
void draw_context_bar(const ViewerView& view, ViewerActions& actions) {
    ImGui::TextUnformatted("Context");
    ImGui::SameLine();
    if (!view.published_bar) {
        text(view.publish_error ? "nothing published" : "nothing published yet");
    } else {
        const ContextBarText& bar = *view.published_bar;
        text(bar.background);
        ImGui::Indent();
        if (bar.point) {
            text(*bar.point);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearPointButton)) {
                actions.clear_point = true;
            }
            tooltip([&] { return std::format("Stop pointing at it: {}", sends(view.publish_error)); });
        }
        if (bar.follow) {
            text(*bar.follow);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearFollowButton)) {
                actions.clear_follow = true;
            }
            tooltip([&] { return std::format("Stop following it: {}", sends(view.publish_error)); });
        }
        ImGui::Unindent();
    }
    if (view.publish_error) {
        // Push and pop must pair; the style stack is checked at the end of the frame.
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        text(std::format("NOT published: {}", *view.publish_error));
        ImGui::PopStyleColor();
    }
}

// The source's state, the latest frame and the viewer's own liveness.
void draw_status(const ViewerView& view) {
    const ViewerState& state = view.state;
    if (state.source_connected) {
        text("source: connected");
    } else if (state.source_error) {
        text(std::format("source: waiting (the last source was dropped: {})", *state.source_error));
    } else {
        text("source: waiting");
    }
    const std::size_t received = state.run ? state.run->frames().size() : 0;
    text(std::format("frames received: {}", received));
    if (received > 0) {
        const Frame& last = state.run->frames().back();
        text(std::format("last frame: #{}  id {}  price {}  qty {}", last.index, last.id, last.price, last.qty));
    }
    if (view.timing.seconds_since_frame) {
        text(std::format("seconds since last frame: {:.1f}", *view.timing.seconds_since_frame));
    }
    text(std::format("viewer paints: {} (keeps rising while the source is stopped)", view.timing.paints));
}

// The prices, as their min/max envelope: reduced to at most one bucket per pixel of the plot's
// width, and each bucket drawn as a vertical stroke from its low to its high, joined to its
// neighbours at their midpoints. Drawn here rather than with ImGui::PlotLines, which samples its
// points to the plot's pixel width and can skip one, and with it a spike; here every bucket is
// drawn, so every price's extremes are on screen. The cost is set by the plot's width, not the
// run's length.
void draw_price_plot(const PriceEnvelope& prices) {
    const float width = std::max(std::floor(ImGui::GetContentRegionAvail().x), 1.0f);
    // An invisible button reserves the plot's space and gives it an ID the UI tests can find.
    ImGui::InvisibleButton(kPricePlot, ImVec2{width, kPlotHeight});
    const ImVec2 top_left = ImGui::GetItemRectMin();
    const ImVec2 bottom_right = ImGui::GetItemRectMax();
    ImDrawList& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled(top_left, bottom_right, ImGui::GetColorU32(ImGuiCol_FrameBg));

    // Reused from paint to paint, so drawing doesn't allocate once it has grown. A function-local
    // static is safe here: only the UI thread draws.
    static std::vector<PriceEnvelope::Bucket> columns;
    prices.reduce_into(static_cast<std::size_t>(width), columns);
    if (columns.empty()) {
        return;
    }
    std::int64_t low = columns.front().low;
    std::int64_t high = columns.front().high;
    for (const PriceEnvelope::Bucket& column : columns) {
        low = std::min(low, column.low);
        high = std::max(high, column.high);
    }
    // In double, since a price difference can exceed what a float holds exactly. A flat run is
    // drawn across the middle.
    const double range = static_cast<double>(high) - static_cast<double>(low);
    const auto y_of = [&](double price) {
        const double fraction = range > 0 ? (price - static_cast<double>(low)) / range : 0.5;
        return static_cast<float>(bottom_right.y - 1.0 - fraction * (kPlotHeight - 2.0));
    };
    const float column_width = width / static_cast<float>(columns.size());
    const ImU32 colour = ImGui::GetColorU32(ImGuiCol_PlotLines);
    ImVec2 previous_mid;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const PriceEnvelope::Bucket& column = columns[i];
        const float x = top_left.x + (static_cast<float>(i) + 0.5f) * column_width;
        const float y_high = y_of(static_cast<double>(column.high));
        const float y_low = std::max(y_of(static_cast<double>(column.low)), y_high + 1.0f);  // at least a pixel
        draw.AddLine(ImVec2{x, y_high}, ImVec2{x, y_low}, colour);
        const ImVec2 mid{x, (y_high + y_low) / 2.0f};
        if (i > 0) {
            draw.AddLine(previous_mid, mid, colour);
        }
        previous_mid = mid;
    }
}

// The frames, one row each. Clicking a row points at its frame; clicking its id follows the id.
// The pointed row and the followed id's rows are highlighted as published, like the bar.
void draw_frame_table(const ViewerView& view, ViewerActions& actions) {
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

    const std::span<const Frame> frames = view.state.run->frames();
    // The clipper asks for only the rows that are visible, so the table costs the same per paint
    // whatever the run's length.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(frames.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const Frame& frame = frames[static_cast<std::size_t>(row)];
            const RowMarks marks = row_marks(frame, view.state.run->number(), view.published);
            ImGui::TableNextRow();
            if (marks.followed) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(kFollowedRowColor));
            }

            ImGui::TableNextColumn();
            // SpanAllColumns makes the whole row the click target; AllowOverlap lets the id's
            // button, drawn on top of it, take its own clicks.
            const Label row_label = point_label(frame);
            if (ImGui::Selectable(row_label.c_str(), marks.pointed,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                actions.point_at = frame;
            }
            report_marked(row_label, marks.pointed);
            tooltip([&] { return std::format("Point at frame #{}: {}", frame.index, sends(view.publish_error)); });

            ImGui::TableNextColumn();
            const Label id_label = follow_label(frame);
            if (ImGui::SmallButton(id_label.c_str())) {
                actions.follow = frame.id;
            }
            report_marked(id_label, marks.followed);
            tooltip([&] {
                return std::format("Follow id {}: highlights every frame with it, and {}", frame.id,
                                   sends(view.publish_error));
            });

            ImGui::TableNextColumn();
            text(Label{"{}", frame.price}.c_str());
            ImGui::TableNextColumn();
            text(Label{"{}", frame.qty}.c_str());
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

RowMarks row_marks(const Frame& frame, std::uint64_t run_number, const std::optional<Context>& published) {
    if (!published || published->source.run_number != run_number) {
        return {};
    }
    return {
        .pointed = published->point && published->point->frame == frame,
        .followed = published->follow && published->follow->id == frame.id,
    };
}

Label point_label(const Frame& frame) { return Label{"{}##point", frame.index}; }

Label follow_label(const Frame& frame) { return Label{"{}##follow{}", frame.id, frame.index}; }

ViewerActions draw_viewer(const ViewerView& view) {
    ViewerActions actions;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(kViewerWindow, nullptr, ImGuiWindowFlags_NoDecoration);

    draw_context_bar(view, actions);
    ImGui::Separator();
    draw_status(view);
    if (view.state.run && !view.state.run->frames().empty()) {
        draw_price_plot(view.state.run->prices());
        draw_frame_table(view, actions);
    }

    ImGui::End();
    return actions;
}

}  // namespace fillscope
