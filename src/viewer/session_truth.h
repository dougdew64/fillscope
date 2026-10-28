// A recorded session's truth, as the viewer shows it (viewer --session DIR): who really sent each
// order and each side of each trade (truth/truth.h), and what each client was
// (tools/sessions/record_session.py writes session.json). The market data says none of this; the
// viewer shows it only beside a replay of the session, marked as the recording's truth, so the
// slicer's trades can be seen before anything tries to find them.
#pragma once

#include "answer/wire.h"
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
    // What each request did, in the order the exchange processed them (DIR/steps.bin, the replay's
    // steps): its bytes, the updates it made and what each client was told. Empty if the session
    // has none.
    std::vector<ReplayStep> steps;
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

// A request, as the session's steps show it beside the update on screen: the request that made the
// update, said in words, and each update it made, so a piece of the slicer's (a trade, the order it
// filled removed, another trade...) can be followed as one event; and the same client's next request
// on the ticker, which for the slicer is the piece's cancel.
struct RequestShown {
    std::size_t number = 0;   // its place in the recording, from 1
    std::string request;      // who sent what: "the slicer (client 6) sent a buy of 30 at 172 on ticker 1"
    std::vector<std::pair<std::uint64_t, std::string>> updates;  // each update it made, and what it was
    std::optional<std::string> next;  // the same client's next request on the ticker, said in words, if any
};

// The request that made update `seq` (from 1), or nothing for update 0 or a session without steps.
[[nodiscard]] std::optional<RequestShown> request_at(const SessionTruth& truth, std::span<const MarketUpdate> updates,
                                                     std::uint64_t seq);

// A request said in words, with what came of it if it made no update ("cancel rejected: the order was
// already gone").
[[nodiscard]] std::string describe_request(const SessionTruth& truth, const ReplayStep& step);

// The first update of the request, among those that made any, nearest update `at`: the next one
// after the request `at` is in, or the previous one before it (its first update, if `at` is past
// it). Nothing if there is none that way.
enum class Way : std::uint8_t { back, forward };
[[nodiscard]] std::optional<std::uint64_t> nearest_request(const SessionTruth& truth, std::uint64_t at, Way way);

// The trade on `ticker` among `updates` (numbered from 1) nearest update `at`, after it (`forward`)
// or before it; only the slicer's, if `slicer` is given, as it says (the incoming order or the
// resting one its client's). Nothing if there is none that way.
[[nodiscard]] std::optional<std::uint64_t> nearest_trade(std::span<const MarketUpdate> updates, std::uint32_t ticker,
                                                         std::uint64_t at, Way way, const SessionTruth* slicer = nullptr);

}  // namespace fillscope
