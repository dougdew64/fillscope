// What the viewer draws, as a function of what it is given, and what the user did while it was
// drawn.
//
// Dear ImGui is an immediate-mode UI: there is no tree of widget objects to build and update. Every
// paint, the code calls ImGui::Button(...) and the like afresh, and a call returns true when the
// user has just clicked it. So drawing and input are one pass. draw_viewer draws everything from
// what it is given and returns what the user did; the caller applies that. Nothing in here changes
// state, which is what lets tests draw it with anything they like and check the result.
#pragma once

#include "viewer/context.h"
#include "viewer/viewer_state.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace fillscope {

// What the user did during one paint. Usually nothing; at most one click's worth.
struct ViewerActions {
    std::optional<Frame> point_at;        // point at this frame
    std::optional<std::uint32_t> follow;  // follow this id
    bool clear_point = false;
    bool clear_follow = false;

    [[nodiscard]] bool any() const noexcept { return point_at || follow || clear_point || clear_follow; }
};

// Applies the user's actions to the selection.
void apply(const ViewerActions& actions, Selection& selection);

// The viewer's own liveness, shown so it is visible that it keeps painting while a source is
// stopped in a debugger.
struct ViewerTiming {
    std::optional<double> seconds_since_frame;  // empty until a frame has arrived
    std::uint64_t paints = 0;
};

// What draw_viewer draws from. The context bar, and the table's highlights, are drawn from
// `published`: the context the file holds, never the live selection, so nothing on screen claims
// the assistant has been given something it hasn't. `published_bar` is the bar's text for it.
// `publish_error` says why the last write failed, if it did.
struct ViewerView {
    const ViewerState& state;
    ViewerTiming timing;
    const std::optional<Context>& published;
    const std::optional<ContextBarText>& published_bar;
    const std::optional<std::string>& publish_error;
};

// The names of the viewer's window and of items inside it, which the UI tests use to find them.
inline constexpr const char* kViewerWindow = "viewer";
inline constexpr const char* kPricePlot = "price";
inline constexpr const char* kFrameTable = "frames";
inline constexpr const char* kClearPointButton = "x##clear_point";
inline constexpr const char* kClearFollowButton = "x##clear_follow";

// A short label, formatted into a fixed buffer: built without allocating, which matters for labels
// built for every visible row on every paint. Text that doesn't fit is cut; the labels here are far
// shorter than the buffer.
class Label {
public:
    // std::format_string checks the format against the arguments' types at compile time.
    template <typename... Args>
    explicit Label(std::format_string<Args...> format, Args&&... args) {
        const auto result = std::format_to_n(text_.data(), text_.size() - 1, format, std::forward<Args>(args)...);
        *result.out = '\0';
    }
    [[nodiscard]] const char* c_str() const noexcept { return text_.data(); }

private:
    std::array<char, 64> text_{};
};

// A frame's row is clicked to point at it, and its id is clicked to follow the id. Everything after
// "##" in a Dear ImGui label is part of the item's ID but not displayed, which keeps each row's
// items distinct while showing just the number.
[[nodiscard]] Label point_label(const Frame& frame);   // "<index>##point"
[[nodiscard]] Label follow_label(const Frame& frame);  // "<id>##follow<index>"

// How a frame's row of run `run_number` is marked in the table: pointed at, and having the
// followed id. Decided from the published context, like the bar, never from the live selection,
// and only when that context is about the same run: frame indices restart at 0 in every run, so an
// earlier run's published point must not mark the new run's frame with the same index. (A function
// of its own, so tests can check the rule; they check the drawn table too.)
struct RowMarks {
    bool pointed = false;
    bool followed = false;
    friend bool operator==(const RowMarks&, const RowMarks&) = default;
};
[[nodiscard]] RowMarks row_marks(const Frame& frame, std::uint64_t run_number, const std::optional<Context>& published);

// Draws the viewer's one full-screen window. Call between ImGui::NewFrame() and ImGui::Render().
[[nodiscard]] ViewerActions draw_viewer(const ViewerView& view);

}  // namespace fillscope
