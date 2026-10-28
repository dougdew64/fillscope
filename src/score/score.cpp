#include "score/score.h"

#include <nlohmann/json.hpp>

#include <format>
#include <fstream>

namespace fillscope {

bool is_target_strategy(std::string_view strategy) noexcept { return strategy == "SLICER" || strategy == "TAKER"; }

std::expected<std::set<std::uint32_t>, std::string> read_targets(const std::filesystem::path& session_json) {
    std::ifstream in{session_json};
    if (!in) {
        return std::unexpected{std::format("can't open {}", session_json.string())};
    }
    std::set<std::uint32_t> targets;
    try {
        for (const auto& client : nlohmann::json::parse(in).at("clients")) {
            if (is_target_strategy(client.at("strategy").get<std::string>())) {
                targets.insert(client.at("client").get<std::uint32_t>());
            }
        }
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected{std::format("{} isn't a session's: {}", session_json.string(), e.what())};
    }
    return targets;
}

std::set<std::uint64_t> target_trades(const Truth& truth, const std::set<std::uint32_t>& targets) {
    std::set<std::uint64_t> updates;
    for (const TradeTruth& trade : truth.trades()) {
        if (targets.contains(trade.incoming_client) || targets.contains(trade.resting_client)) {
            updates.insert(trade.update);
        }
    }
    return updates;
}

std::expected<Score, std::string> score_flags(std::span<const MarketUpdate> updates, const Truth& truth,
                                               const std::set<std::uint32_t>& targets, const std::set<std::uint64_t>& flagged) {
    const auto is_trade = [&](std::uint64_t update) {
        return update >= 1 && update <= updates.size() && updates[update - 1].type == UpdateType::trade;
    };
    for (const std::uint64_t update : flagged) {
        if (!is_trade(update)) {
            return std::unexpected{std::format("update {} is flagged, but isn't a trade", update)};
        }
    }
    // Every trade in the market data is the truth's, and the truth's only those, so none goes unscored.
    std::size_t market_trades = 0;
    for (const MarketUpdate& update : updates) {
        market_trades += update.type == UpdateType::trade ? 1 : 0;
    }
    if (market_trades != truth.trades().size()) {
        return std::unexpected{
            std::format("the market data has {} trades, its truth {}: not this market data's truth", market_trades, truth.trades().size())};
    }

    Score score;
    for (const TradeTruth& trade : truth.trades()) {
        if (!is_trade(trade.update)) {
            return std::unexpected{std::format("the truth has a trade at update {}, which the market data doesn't", trade.update)};
        }
        const bool target = targets.contains(trade.incoming_client) || targets.contains(trade.resting_client);
        const bool flag = flagged.contains(trade.update);
        for (Tally* tally : {&score.all, &score.by_ticker[updates[trade.update - 1].ticker]}) {
            ++tally->trades;
            tally->targets += target ? 1 : 0;
            tally->hits += target && flag ? 1 : 0;
            tally->misses += target && !flag ? 1 : 0;
            tally->false_alarms += !target && flag ? 1 : 0;
        }
    }
    return score;
}

}  // namespace fillscope
