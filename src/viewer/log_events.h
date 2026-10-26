// What the viewer's log records of what happened and what the user did (viewer_log.h): lines found
// by comparing the viewer's state with what it was at the last paint, and a line for each paint's
// actions. Kept apart from the loop that writes them, so they can be tested without a window.
#pragma once

#include "viewer/answer_state.h"
#include "viewer/viewer_state.h"
#include "viewer/viewer_view.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fillscope {

// What the log last said of the state, so it says each change once.
struct LogWatch {
    BookBuilder::Status status = BookBuilder::Status::waiting;
    std::size_t recoveries = 0;
    std::optional<std::string> feed_error;
    std::optional<FileStamp> answer_stamp;
    std::optional<std::string> answer_problem;
};

// The lines for what changed since `watch` was last brought up to date, which this does: the feed
// (live, the feed's books, whatever the screen shows) going live, starting to recover (and why),
// each rebuild from a snapshot, a new error receiving; an answer read and shown, or refused (and
// why). Nothing for a state that hasn't changed.
[[nodiscard]] std::vector<std::string> log_events(const ViewerState& state, LogWatch& watch);

// A line for what the user did in one paint ("user: points at order 7 of ticker 0"), several
// actions joined; nothing if the user did nothing.
[[nodiscard]] std::optional<std::string> describe(const ViewerActions& actions);

}  // namespace fillscope
