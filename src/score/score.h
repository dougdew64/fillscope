// The detector's check (FEATURES.md, item 13): which trades of a recorded session are targets, and,
// given the trades a detector flagged, its hits, misses and false alarms against them. The score is
// defined here before the detector is written, and not by its author, so the detector can't be
// shaped to it. A target is any client that trades repeatedly at one side and size: the planted
// slicer, and the liquidity taker, which always trades its one clip. A trade is a target's if a
// target was on either side of it, its incoming order or its resting one, as the truth says.
//
// Only a replay is scored: the truth (truth/truth.h) comes from the exchange's recording, and the
// market data names no client. A detector's flags name trades by their update in the market data,
// which is all a detector can see. Nothing here calls a detector; when one exists, an adapter runs
// it over a replay and gives its flags to score_flags.
#pragma once

#include "feed/market_update.h"
#include "truth/truth.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace fillscope {

// Whether a client run with this strategy, as trading_main and session.json name it ("SLICER",
// "TAKER", "MAKER", "RANDOM"), is a target.
[[nodiscard]] bool is_target_strategy(std::string_view strategy) noexcept;

// The session's target clients, from its session.json (tools/sessions/record_session.py writes it),
// or why they can't be read.
[[nodiscard]] std::expected<std::set<std::uint32_t>, std::string> read_targets(const std::filesystem::path& session_json);

// The updates of the trades a target was on a side of.
[[nodiscard]] std::set<std::uint64_t> target_trades(const Truth& truth, const std::set<std::uint32_t>& targets);

// The count, over some trades, of each outcome: a hit is flagged and a target's, a miss a target's
// not flagged, a false alarm flagged and not a target's.
struct Tally {
    std::size_t trades = 0;
    std::size_t targets = 0;  // the targets' trades: hits and misses
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t false_alarms = 0;

    friend bool operator==(const Tally&, const Tally&) = default;
};

// A detector's score over a session: in all, and on each ticker that had a trade.
struct Score {
    Tally all;
    std::map<std::uint32_t, Tally> by_ticker;

    friend bool operator==(const Score&, const Score&) = default;
};

// The score of the trades `flagged`, each named by its update, among the session's `updates`
// (numbered from 1) with its `truth` and `targets`; or why it can't be scored: a flag on an update
// that isn't a trade, or a truth whose trades aren't the market data's.
[[nodiscard]] std::expected<Score, std::string> score_flags(std::span<const MarketUpdate> updates, const Truth& truth,
                                                             const std::set<std::uint32_t>& targets,
                                                             const std::set<std::uint64_t>& flagged);

}  // namespace fillscope
