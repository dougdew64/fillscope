#include "viewer/viewer_view.h"

#include <imgui.h>

#include <algorithm>
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
    if (!view.published) {
        text("nothing published yet");
    } else {
        const ContextBarText bar = bar_text(*view.published);
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

// The prices, as their min/max envelope reduced to half the plot's width in buckets: each bucket is
// drawn as its low then its high, two points, so the line passes through every bucket's extremes
// and no spike is lost, at a cost set by the plot's width rather than the run's length.
void draw_price_plot(const PriceEnvelope& prices) {
    const float width = ImGui::GetContentRegionAvail().x;
    const auto buckets = prices.reduced(static_cast<std::size_t>(std::max(width / 2.0f, 1.0f)));
    std::vector<float> points;
    points.reserve(buckets.size() * 2);
    float low = buckets.front().low;
    float high = buckets.front().high;
    for (const PriceEnvelope::Bucket& bucket : buckets) {
        points.push_back(bucket.low);
        points.push_back(bucket.high);
        low = std::min(low, bucket.low);
        high = std::max(high, bucket.high);
    }
    // An explicit scale, so PlotLines doesn't scan the points for it; a flat run gets a unit range.
    if (low == high) {
        low -= 0.5f;
        high += 0.5f;
    }
    ImGui::PlotLines(kPricePlot, points.data(), static_cast<int>(points.size()), 0, nullptr, low, high,
                     ImVec2{-1.0f, kPlotHeight});
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
            const RowMarks marks = row_marks(frame, view.published);
            ImGui::TableNextRow();
            if (marks.followed) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(kFollowedRowColor));
            }

            ImGui::TableNextColumn();
            // SpanAllColumns makes the whole row the click target; AllowOverlap lets the id's
            // button, drawn on top of it, take its own clicks.
            if (ImGui::Selectable(point_label(frame).c_str(), marks.pointed,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                actions.point_at = frame;
            }
            tooltip([&] { return std::format("Point at frame #{}: {}", frame.index, sends(view.publish_error)); });

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(follow_label(frame).c_str())) {
                actions.follow = frame.id;
            }
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

RowMarks row_marks(const Frame& frame, const std::optional<Context>& published) {
    if (!published) {
        return {};
    }
    return {
        .pointed = published->point && published->point->frame.index == frame.index,
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
