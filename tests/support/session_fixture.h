// A recorded session made by hand for the viewer's tests (viewer/session_truth.h): its market data,
// its truth and its session.json, as tools/sessions/record_session.py and the back end's replay
// would write them, from updates and a truth given here. The truth must be what the back end would
// have said of those updates; session_system_tests checks it does, on a real session.
#pragma once

#include "feed/market_update.h"
#include "support/answer_fixture.h"
#include "truth/truth.h"
#include "viewer/session_truth.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fillscope::test {

inline MarketUpdate session_update(std::uint64_t seq, UpdateType type, std::uint64_t order_id, std::uint32_t ticker, Side side,
                                   std::int64_t price, std::uint32_t qty) {
    return {.seq = seq, .type = type, .order_id = order_id, .ticker = ticker, .side = side, .price = price, .qty = qty,
            .priority = order_id};
}

// Clients 1 (the market maker), 3 (the taker), 5 (the random client) and 6 (the slicer), on ticker 1:
//   1-3   orders 1 (the maker's sell at 105), 2 (a random sell at 106), 3 (a random buy at 100)
//   4-5   the slicer's order 4 buys 10, taking all of order 1: the slicer took
//   6     the slicer's order 5 rests, a buy of 5 at 105
//   7-8   a random sell, order 6, meets it: the slicer's resting order was met
//   9-10  the taker's order 7 buys 4 of order 2: the slicer wasn't in it
// and on ticker 2:
//   11-13 a random sell, order 1, met by the slicer's order 2: a trade the panel for ticker 1 leaves out
inline std::vector<MarketUpdate> session_updates() {
    using U = UpdateType;
    return {session_update(1, U::add, 1, 1, Side::sell, 105, 10),  session_update(2, U::add, 2, 1, Side::sell, 106, 10),
            session_update(3, U::add, 3, 1, Side::buy, 100, 10),   session_update(4, U::trade, 0, 1, Side::buy, 105, 10),
            session_update(5, U::cancel, 1, 1, Side::sell, 105, 0), session_update(6, U::add, 5, 1, Side::buy, 105, 5),
            session_update(7, U::trade, 0, 1, Side::sell, 105, 5),  session_update(8, U::cancel, 5, 1, Side::buy, 105, 0),
            session_update(9, U::trade, 0, 1, Side::buy, 106, 4),   session_update(10, U::modify, 2, 1, Side::sell, 106, 6),
            session_update(11, U::add, 1, 2, Side::sell, 50, 3),   session_update(12, U::trade, 0, 2, Side::buy, 50, 3),
            session_update(13, U::cancel, 1, 2, Side::sell, 50, 0)};
}

inline Truth session_truth_of() {
    return Truth{"requests.rec",
                 {{1, 1, 1}, {1, 2, 5}, {1, 3, 5}, {1, 4, 6}, {1, 5, 6}, {1, 6, 5}, {1, 7, 3}, {2, 1, 5}, {2, 2, 6}},
                 {{.update = 4, .incoming_client = 6, .incoming_order_id = 4, .resting_client = 1, .resting_order_id = 1},
                  {.update = 7, .incoming_client = 5, .incoming_order_id = 6, .resting_client = 6, .resting_order_id = 5},
                  {.update = 9, .incoming_client = 3, .incoming_order_id = 7, .resting_client = 5, .resting_order_id = 2},
                  {.update = 12, .incoming_client = 6, .incoming_order_id = 2, .resting_client = 5, .resting_order_id = 1}}};
}

inline nlohmann::json session_json() {
    return {{"clients",
             {{{"client", 1}, {"strategy", "MAKER"}},
              {{"client", 3}, {"strategy", "TAKER"}},
              {{"client", 5}, {"strategy", "RANDOM"}},
              {{"client", 6},
               {"strategy", "SLICER"},
               {"orders",
                {{{"kind", "equal pieces, regular gaps"}, {"ticker", 1}, {"side", "BUY"}, {"total", 15}, {"piece", 10},
                  {"gap", 3}, {"gap_jitter", 0}, {"size_spread", 0}}}}}}}};
}

// The session, as read_session gives it.
inline Session hand_session() {
    return Session{.market_data = {.recording = "requests.rec", .updates = session_updates()},
                   .truth = {.directory = "/sessions/hand",
                             .truth = session_truth_of(),
                             .roles = {{1, "maker"}, {3, "taker"}, {5, "random"}, {6, "slicer"}},
                             .slicers = {6},
                             .slices = {{1, "buys 15 in pieces of 10, each 3 updates on this ticker after the last (equal pieces, "
                                            "regular gaps)"}}}};
}

// Writes the session's three files into `directory`.
inline void write_session(const std::filesystem::path& directory, const nlohmann::json& json = session_json(),
                          const Truth& truth = session_truth_of()) {
    std::filesystem::create_directories(directory);
    write_market_data(directory / "market.dat", "requests.rec", session_updates());
    const auto bytes = encode_truth_file(truth);
    std::ofstream{directory / "truth.bin", std::ios::binary}.write(reinterpret_cast<const char*>(bytes.data()),
                                                                     static_cast<std::streamsize>(bytes.size()));
    std::ofstream{directory / "session.json"} << json.dump();
}

}  // namespace fillscope::test
