// A recorded session's truth, as the viewer shows it (viewer --session DIR): who really sent each
// order and each side of each trade (truth/truth.h), and what each client was
// (tools/sessions/record_session.py writes session.json). The market data says none of this; the
// viewer shows it only beside a replay of the session, marked as the recording's truth, so the
// slicer's trades can be seen before anything tries to find them.
#pragma once

#include "feed/market_data_file.h"
#include "feed/market_update.h"
#include "truth/truth.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace fillscope {

struct SessionTruth {
    std::filesystem::path directory;  // the session's, as an absolute path
    Truth truth;
    std::map<std::uint32_t, std::string> roles;    // each client's strategy, in a word: "maker", "taker", "random", "slicer"
    std::set<std::uint32_t> slicers;               // the clients whose trades are coloured
    std::map<std::uint32_t, std::string> slices;   // by ticker: the slicer's order there, described
};

// A session's market data and truth: DIR/market.dat, DIR/truth.bin and DIR/session.json, as
// record_session.py writes them; or why they can't be read, or don't agree (truth_error).
struct Session {
    MarketDataFile market_data;
    SessionTruth truth;
};
[[nodiscard]] std::expected<Session, std::string> read_session(const std::filesystem::path& directory);

// Why `truth` isn't the truth of `market_data`, if it isn't: replayed from another recording, or its
// trades not exactly the market data's.
[[nodiscard]] std::optional<std::string> truth_error(const MarketDataFile& market_data, const Truth& truth);

// A client, named for the book's column: "6 (slicer)", or "6" if its role isn't known.
[[nodiscard]] std::string client_name(const SessionTruth& truth, std::uint32_t client);

// One trade, as the trades panel draws it: where (its update, its price), how much, which side came
// in, and whether the slicer was in it: its incoming order, which took, or its resting one, met.
struct TradeDot {
    enum class Slicer : std::uint8_t { none, took, met };
    std::uint64_t update = 0;
    std::int64_t price = 0;
    std::uint32_t qty = 0;
    Side side = Side::none;  // the incoming order's
    Slicer slicer = Slicer::none;

    friend bool operator==(const TradeDot&, const TradeDot&) = default;
};

// The trades on `ticker` among `updates` (numbered from 1) from update `from` to update `to`, in
// order, each marked from `truth`.
[[nodiscard]] std::vector<TradeDot> trade_dots(std::span<const MarketUpdate> updates, const SessionTruth& truth,
                                               std::uint32_t ticker, std::uint64_t from, std::uint64_t to);

}  // namespace fillscope
