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
    std::optional<PointTarget> point_at;      // point at this order or level
    std::optional<OrderRef> follow;           // follow this order
    std::optional<std::uint32_t> show_ticker; // show this ticker's book
    std::optional<std::uint64_t> replay_to;   // in a replay: the books after this update
    bool clear_point = false;
    bool clear_follow = false;

    [[nodiscard]] bool any() const noexcept {
        return point_at || follow || show_ticker || replay_to || clear_point || clear_follow;
    }
};

// Applies the user's actions: to the selection, and to which ticker is shown.
void apply(const ViewerActions& actions, Selection& selection, ViewerState& state);

// The viewer's own liveness, shown so it is visible that it keeps painting while the exchange is
// stopped in a debugger.
struct ViewerTiming {
    std::optional<double> seconds_since_update;  // empty until an update has arrived
    std::uint64_t paints = 0;
};

// What draw_viewer draws from. The context bar, and the book's highlights, are drawn from
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
inline constexpr const char* kBookTable = "book";
inline constexpr const char* kHistoryTable = "history";
inline constexpr const char* kContextHeading = "Context, as published for the assistant:";
inline constexpr const char* kClearPointButton = "x##clear_point";
inline constexpr const char* kClearFollowButton = "x##clear_follow";
// A replay's controls: to its start, back one update, forward one, to its end, and a slider.
inline constexpr const char* kReplayStart = "|<##replay_start";
inline constexpr const char* kReplayBack = "<##replay_back";
inline constexpr const char* kReplayForward = ">##replay_forward";
inline constexpr const char* kReplayEnd = ">|##replay_end";
inline constexpr const char* kReplaySlider = "##replay_position";

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

// The clickable items' labels. Everything after "##" in a Dear ImGui label is part of the item's ID
// but not displayed, which keeps items distinct while showing just what matters.
[[nodiscard]] Label ticker_label(std::uint32_t ticker);  // "<ticker>##ticker": show its book
[[nodiscard]] Label level_label(const LevelRef& level);  // "<price>##level<buy|sell>": point at the level
[[nodiscard]] Label order_label(const Order& order);     // "<side>##order<id>": point at the order (its row)
[[nodiscard]] Label follow_label(const Order& order);    // "<id>##follow": follow the order

// How the book's items are marked: decided from the published context, like the bar, never from
// the live selection. (Functions of their own, so tests can check the rule; they check the drawn
// book too.)
[[nodiscard]] bool level_marked(const LevelRef& level, const std::optional<Context>& published);
[[nodiscard]] bool order_pointed(const OrderRef& order, const std::optional<Context>& published);
[[nodiscard]] bool order_followed(const OrderRef& order, const std::optional<Context>& published);

// Draws the viewer's one full-screen window. Call between ImGui::NewFrame() and ImGui::Render().
[[nodiscard]] ViewerActions draw_viewer(const ViewerView& view);

}  // namespace fillscope
