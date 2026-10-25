// What the viewer has received: the exchange's books, kept in step with its market data
// (feed/book_builder.h), and how the market data is arriving, live or from a replay.
#pragma once

#include "feed/book_builder.h"
#include "viewer/answer_state.h"
#include "feed/market_update.h"

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

struct ViewerState {
    BookBuilder books;
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
};

}  // namespace fillscope
