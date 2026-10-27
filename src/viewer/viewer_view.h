// What the viewer draws, as a function of what it is given, and what the user did while it was
// drawn.
//
// Dear ImGui is an immediate-mode UI: there is no tree of widget objects to build and update. Every
// paint, the code calls ImGui::Button(...) and the like afresh, and a call returns true when the
// user has just clicked it. So drawing and input are one pass. draw_viewer draws everything from
// what it is given and returns what the user did; the caller applies that. Nothing in here changes
// state, which is what lets tests draw it with anything they like and check the result.
#pragma once

#include "feed/time_shift.h"
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
    std::optional<std::size_t> answer_to;     // in an answer: show this frame
    std::optional<std::size_t> choose;        // with a course: show this (0: your questions; a lesson, from 1)
    bool clear_point = false;
    bool clear_follow = false;
    // Live, time-shifting (feed/time_shift.h): pause, play from the update shown, show the books as
    // of an update kept (paused), follow live again.
    bool pause = false;
    bool play = false;
    std::optional<std::uint64_t> go_to;
    bool go_live = false;

    [[nodiscard]] bool any() const noexcept {
        return point_at || follow || show_ticker || replay_to || answer_to || choose || clear_point || clear_follow || pause ||
               play || go_to || go_live;
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
    // Live, what the viewer keeps of the market data, for the time-shifting controls; null otherwise.
    const TimeShiftBuffer* kept = nullptr;
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
// Live, the time-shifting controls: pause, play, to the oldest kept, back one, forward one, a slider
// over what is kept, and back to live.
inline constexpr const char* kShiftPause = "pause##shift_pause";
inline constexpr const char* kShiftPlay = "play##shift_play";
inline constexpr const char* kShiftOldest = "|<##shift_oldest";
inline constexpr const char* kShiftBack = "<##shift_back";
inline constexpr const char* kShiftForward = ">##shift_forward";
inline constexpr const char* kShiftSlider = "##shift_position";
inline constexpr const char* kShiftLive = "live##shift_live";
// Live, the region of the market pane above the time-shifting controls: the tickers, the book, the
// history.
inline constexpr const char* kLiveMarket = "live_market";
// An answer's pane, the market beside it, and the answer's controls: to its start, back a frame,
// forward a frame, to its end.
inline constexpr const char* kAnswerPane = "answer";
inline constexpr const char* kMarketPane = "market";
inline constexpr const char* kAnswerStart = "|<##answer_start";
inline constexpr const char* kAnswerBack = "<##answer_back";
inline constexpr const char* kAnswerForward = ">##answer_forward";
inline constexpr const char* kAnswerEnd = ">|##answer_end";
// The trades panel, beside the book: each trade a dot, its update across and its price up. A dot's
// item is "##trade<update>"; the UI tests read what it shows from its report (trade_report).
inline constexpr const char* kTradesPanel = "trades";
// How many updates the panel spans, ending at the one shown.
inline constexpr std::uint64_t kTradesWindow = 1200;
// The choice of what the pane shows, with a course: your latest question, or a lesson.
inline constexpr const char* kLessonChoice = "##lesson";

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

// What a trade's dot reports of itself to the UI tests: "<update>: <qty> at <price>, <none|took|met>",
// the last whether the slicer was in it, as the truth says.
[[nodiscard]] Label trade_report(const TradeDot& dot);

// How the book's items are marked: decided from the published context, like the bar, never from
// the live selection. (Functions of their own, so tests can check the rule; they check the drawn
// book too.)
[[nodiscard]] bool level_marked(const LevelRef& level, const std::optional<Context>& published);
[[nodiscard]] bool order_pointed(const OrderRef& order, const std::optional<Context>& published);
[[nodiscard]] bool order_followed(const OrderRef& order, const std::optional<Context>& published);

// The viewer's look: text twice Dear ImGui's default size, in its scalable font (the pixel font it
// would otherwise choose is drawn for 13 pixels and blurs when scaled). Call once, after
// ImGui::CreateContext() and before the first frame.
void apply_viewer_look();

// The window's size, in screen coordinates, with room for that text: 1800 by 1400, or the screen's
// work area (`area_width` by `area_height`) where that is smaller; 1800 by 1400 if the area is
// unknown (zero).
struct WindowSize {
    int width;
    int height;
};
[[nodiscard]] WindowSize window_size(int area_width, int area_height);

// Draws the viewer's one full-screen window. Call between ImGui::NewFrame() and ImGui::Render().
[[nodiscard]] ViewerActions draw_viewer(const ViewerView& view);

}  // namespace fillscope
