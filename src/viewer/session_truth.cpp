#include "viewer/session_truth.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>

namespace fillscope {

namespace {

// A strategy's name on trading_main's command line, as session.json gives it, in a word.
std::string role_of(const std::string& strategy) {
    if (strategy == "MAKER") {
        return "maker";
    }
    if (strategy == "TAKER") {
        return "taker";
    }
    if (strategy == "RANDOM") {
        return "random";
    }
    if (strategy == "SLICER") {
        return "slicer";
    }
    return strategy;
}

// The slicer's order on its ticker, as session.json describes it.
std::string describe_slice(const nlohmann::json& order) {
    const std::uint32_t spread = order.at("size_spread");
    const std::uint32_t jitter = order.at("gap_jitter");
    return std::format("{} {} in pieces of {}{}, each {}{} updates on this ticker after the last ({})",
                       order.at("side") == "BUY" ? "buys" : "sells", order.at("total").get<std::uint32_t>(),
                       order.at("piece").get<std::uint32_t>(), spread > 0 ? std::format(" ± {}", spread) : std::string{},
                       order.at("gap").get<std::uint32_t>(), jitter > 0 ? std::format(" ± {}", jitter) : std::string{},
                       order.at("kind").get<std::string>());
}

}  // namespace

std::expected<Session, std::string> read_session(const std::filesystem::path& directory) {
    auto market_data = read_market_data_file(directory / "market.dat");
    if (!market_data) {
        return std::unexpected{market_data.error()};
    }
    auto truth = read_truth_file(directory / "truth.bin");
    if (!truth) {
        return std::unexpected{truth.error()};
    }
    if (const auto error = truth_error(*market_data, *truth)) {
        return std::unexpected{std::format("{}: {}", (directory / "truth.bin").string(), *error)};
    }
    const std::filesystem::path json_file = directory / "session.json";
    std::ifstream in{json_file};
    if (!in) {
        return std::unexpected{std::format("can't open {}", json_file.string())};
    }
    SessionTruth session{.directory = std::filesystem::absolute(directory), .truth = std::move(*truth), .roles = {}, .slicers = {},
                         .slices = {}};
    try {
        const auto json = nlohmann::json::parse(in);
        for (const auto& client : json.at("clients")) {
            const std::uint32_t id = client.at("client");
            const std::string strategy = client.at("strategy");
            session.roles[id] = role_of(strategy);
            if (strategy == "SLICER") {
                session.slicers.insert(id);
                for (const auto& order : client.at("orders")) {
                    session.slices[order.at("ticker").get<std::uint32_t>()] = describe_slice(order);
                }
            }
        }
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected{std::format("{} isn't a session's: {}", json_file.string(), e.what())};
    }
    return Session{.market_data = std::move(*market_data), .truth = std::move(session)};
}

std::optional<std::string> truth_error(const MarketDataFile& market_data, const Truth& truth) {
    if (truth.recording() != market_data.recording) {
        return std::format("it was replayed from {}, the market data from {}", truth.recording(), market_data.recording);
    }
    std::size_t trades = 0;
    for (const MarketUpdate& update : market_data.updates) {
        if (update.type == UpdateType::trade) {
            ++trades;
            if (truth.trade_at(update.seq) == nullptr) {
                return std::format("it has no trade at update {}, which is one", update.seq);
            }
        }
    }
    if (truth.trades().size() != trades) {
        return std::format("it has {} trades, the market data {}", truth.trades().size(), trades);
    }
    return std::nullopt;
}

std::string client_name(const SessionTruth& truth, std::uint32_t client) {
    const auto role = truth.roles.find(client);
    return role != truth.roles.end() ? std::format("{} ({})", client, role->second) : std::format("{}", client);
}

std::vector<TradeDot> trade_dots(std::span<const MarketUpdate> updates, const SessionTruth& truth, std::uint32_t ticker,
                                 std::uint64_t from, std::uint64_t to) {
    std::vector<TradeDot> dots;
    from = std::max<std::uint64_t>(from, 1);
    to = std::min<std::uint64_t>(to, updates.size());
    for (std::uint64_t seq = from; seq <= to; ++seq) {
        const MarketUpdate& update = updates[seq - 1];
        if (update.type != UpdateType::trade || update.ticker != ticker) {
            continue;
        }
        TradeDot dot{.update = seq, .price = update.price, .qty = update.qty, .side = update.side, .slicer = TradeDot::Slicer::none};
        if (const TradeTruth* trade = truth.truth.trade_at(seq)) {
            if (truth.slicers.contains(trade->incoming_client)) {
                dot.slicer = TradeDot::Slicer::took;
            } else if (truth.slicers.contains(trade->resting_client)) {
                dot.slicer = TradeDot::Slicer::met;
            }
        }
        dots.push_back(dot);
    }
    return dots;
}

}  // namespace fillscope
