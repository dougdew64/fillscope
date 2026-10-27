// What the viewer has received: the exchange's books, kept in step with its market data
// (feed/book_builder.h), and how the market data is arriving, live or from a replay.
#pragma once

#include "feed/book_builder.h"
#include "feed/market_update.h"
#include "feed/time_shift.h"
#include "viewer/answer_state.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fillscope {

// A replay's market data (feed/market_data_file.h): the books are built from its updates 1 to N, so
// they are exactly the exchange's after update N of the recorded run, and N can be anything.
struct Replay {
    std::filesystem::path file;  // the market data file, as an absolute path
    std::string recording;       // the recording it was replayed from, as exchange_replay was given it
    std::vector<MarketUpdate> updates;
    // The whole run, held as a live feed's is: its moments, as kept (TimeShiftBuffer::from_replay), and
    // the books after all of it, whose histories the books shown have up to their update. Both stop
    // before an update the books can't apply: a replay has no snapshots to recover with.
    TimeShiftBuffer kept;
    BookBuilder books;

    // A replay of `updates`, numbered 1, 2, 3, ... (read_market_data_file checks), with its run held.
    [[nodiscard]] static Replay of(std::filesystem::path file, std::string recording, std::vector<MarketUpdate> updates);
};

// Live, the moment the screen shows when it isn't following the newest update (feed/time_shift.h),
// as a television recorder shows a moment of what it recorded: paused there, or playing on from it at
// the pace the updates arrived, behind live.
struct TimeShift {
    std::uint64_t shown = 0;  // the books shown are as of this update
    bool playing = false;
    // Playing: the arrival time of the update shown when play began, and when it began. The update
    // shown is then the last whose arrival is no later than play_from plus the time since play_since,
    // so it stays as far behind live as it was.
    std::chrono::steady_clock::time_point play_from{};
    std::chrono::steady_clock::time_point play_since{};
    // How far behind: the newest update kept, and the seconds between its arrival and the shown
    // update's (the viewer's arrival times, not the exchange's: the market data carries none).
    std::uint64_t newest = 0;
    std::optional<double> behind_seconds;

    friend bool operator==(const TimeShift&, const TimeShift&) = default;
};

struct ViewerState {
    // The books shown. Live, they follow `live` update by update, unless time-shifted, when they are
    // the books as of the update shown. They keep no histories: shown_history() has them.
    BookBuilder books = BookBuilder::keeping_no_histories();
    // Live: the feed's books, always current, from which its health is shown and published.
    BookBuilder live;
    // Live: set while the screen shows a moment that isn't the newest.
    std::optional<TimeShift> time_shift;
    // Live: the file what is kept is written to while the viewer runs (feed/kept_file.h), and whether
    // the books shown are a moment in it, so the context can give their address there.
    std::optional<std::filesystem::path> kept_file;
    bool books_kept = false;
    // The ticker whose book is shown: the user's choice of view, which the context publishes as
    // background (the assistant should know what the user is looking at).
    std::uint32_t shown_ticker = 0;
    // Why the last datagram couldn't be received or decoded, if one couldn't.
    std::optional<std::string> feed_error;
    // Set when the viewer shows a replay rather than the live exchange.
    std::optional<Replay> replay;
    // Set when the viewer shows answers (--answer): the answer file it watches, and what it shows
    // of it. An answer's market data is shown as a replay, stepped a frame at a time.
    std::optional<AnswerShown> answer;
    // Set when it also offers a course: its lessons, and which is chosen.
    std::optional<CourseChoice> course;
};

// What the books shown have seen of an order: the feed's history of it (the replay's, or the live
// feed's) up to the update shown. One set of histories, however far back the books shown are.
[[nodiscard]] std::span<const OrderEvent> shown_history(const ViewerState& state, std::uint32_t ticker, std::uint64_t order_id);

}  // namespace fillscope
