// What the viewer has received: the exchange's books, kept in step with its market data
// (feed/book_builder.h), and how the market data is arriving, live or from a replay.
#pragma once

#include "feed/book_builder.h"
#include "viewer/answer_state.h"
#include "feed/market_update.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fillscope {

// A replay's market data (feed/market_data_file.h): the books are built from its updates 1 to N, so
// they are exactly the exchange's after update N of the recorded run, and N can be anything.
struct Replay {
    std::filesystem::path file;  // the market data file, as an absolute path
    std::string recording;       // the recording it was replayed from, as exchange_replay was given it
    std::vector<MarketUpdate> updates;
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
    // the books as of the update shown.
    BookBuilder books;
    // Live: the feed's books, always current, from which its health is shown and published.
    BookBuilder live;
    // Live: set while the screen shows a moment that isn't the newest.
    std::optional<TimeShift> time_shift;
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

}  // namespace fillscope
