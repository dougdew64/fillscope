#include "viewer/session_truth.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <cstring>
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

// A client named by its role: "the slicer (client 6)", or "client 9" if its role isn't known.
std::string who(const SessionTruth& truth, std::uint32_t client) {
    const auto role = truth.roles.find(client);
    if (role == truth.roles.end()) {
        return std::format("client {}", client);
    }
    const std::string& r = role->second;
    const std::string name = r == "maker" ? "the market maker" : r == "taker" ? "the liquidity taker" : r == "random" ? "the random client"
                             : r == "slicer"                                  ? "the slicer"
                                                                              : r;
    return std::format("{} (client {})", name, client);
}

// A request's fields, from its bytes (answer/wire.h has the layout): type (1 a new order, 2 a
// cancel), client, ticker, the client's order id, side, price, quantity.
struct RequestFields {
    std::uint8_t type = 0;
    std::uint32_t client = 0;
    std::uint32_t ticker = 0;
    std::uint64_t order_id = 0;
    std::int8_t side = 0;
    std::int64_t price = 0;
    std::uint32_t qty = 0;
};

RequestFields decode(const RequestBytes& bytes) {
    RequestFields r;
    std::size_t at = 0;
    const auto take = [&](auto& field) {
        std::memcpy(&field, bytes.data() + at, sizeof field);
        at += sizeof field;
    };
    take(r.type);
    take(r.client);
    take(r.ticker);
    take(r.order_id);
    take(r.side);
    take(r.price);
    take(r.qty);
    return r;
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
    auto steps = read_steps_file(directory / "steps.bin");
    if (!steps) {
        return std::unexpected{steps.error()};
    }
    // Each update made by exactly one request, in order: the steps' updates are the market data's.
    std::uint64_t next = 1;
    for (const ReplayStep& step : steps->steps) {
        if (step.first_update != next) {
            return std::unexpected{std::format("{}: a step's updates start at {}, not {}", (directory / "steps.bin").string(),
                                               step.first_update, next)};
        }
        next += step.update_count;
    }
    if (next != market_data->updates.size() + 1) {
        return std::unexpected{std::format("{}: its steps made {} updates, the market data has {}", (directory / "steps.bin").string(),
                                           next - 1, market_data->updates.size())};
    }
    const std::filesystem::path json_file = directory / "session.json";
    std::ifstream in{json_file};
    if (!in) {
        return std::unexpected{std::format("can't open {}", json_file.string())};
    }
    SessionTruth session{.directory = std::filesystem::absolute(directory), .truth = std::move(*truth), .roles = {}, .slicers = {},
                         .slices = {}, .steps = std::move(steps->steps)};
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

namespace {

// The steps' index of the request that made update `seq`, if one did.
std::optional<std::size_t> step_of(const SessionTruth& truth, std::uint64_t seq) {
    const auto& steps = truth.steps;
    // The last step starting at or before `seq`; steps that made no update share their start with
    // the next one, so the last of them is the one that made it, if any did.
    const auto after = std::ranges::upper_bound(steps, seq, {}, &ReplayStep::first_update);
    if (seq == 0 || after == steps.begin()) {
        return std::nullopt;
    }
    const auto step = std::prev(after);
    if (seq >= step->first_update + step->update_count) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(step - steps.begin());
}

}  // namespace

std::string describe_request(const SessionTruth& truth, const ReplayStep& step) {
    const RequestFields r = decode(step.request);
    if (r.type == 1) {
        return std::format("{} sent a {} of {} at {} on ticker {}", who(truth, r.client), r.side == 1 ? "buy" : "sell", r.qty, r.price,
                           r.ticker);
    }
    for (const ClientResponse& response : step.responses) {
        if (response.type == ResponseType::canceled) {
            return std::format("{} cancelled its order {} on ticker {}", who(truth, r.client), response.market_order_id, r.ticker);
        }
        if (response.type == ResponseType::cancel_rejected) {
            return std::format("{} tried to cancel an order on ticker {}, rejected: it was already gone", who(truth, r.client), r.ticker);
        }
    }
    return std::format("{} sent a cancel on ticker {}", who(truth, r.client), r.ticker);
}

std::optional<RequestShown> request_at(const SessionTruth& truth, std::span<const MarketUpdate> updates, std::uint64_t seq) {
    const auto index = step_of(truth, seq);
    if (!index || seq > updates.size()) {
        return std::nullopt;
    }
    const ReplayStep& step = truth.steps[*index];
    const RequestFields request = decode(step.request);
    RequestShown shown{.number = *index + 1, .request = describe_request(truth, step), .updates = {}, .next = std::nullopt};
    std::set<std::uint64_t> traded;  // the resting orders this request's trades met, by order id
    for (std::uint64_t u = step.first_update; u < step.first_update + step.update_count; ++u) {
        const MarketUpdate& update = updates[u - 1];
        std::string said;
        switch (update.type) {
            case UpdateType::add:
                said = std::format("order {} added: a {} of {} at {}, resting in the book", update.order_id,
                                   update.side == Side::buy ? "buy" : "sell", update.qty, update.price);
                break;
            case UpdateType::trade:
                if (const TradeTruth* trade = truth.truth.trade_at(u)) {
                    said = std::format("trade: {} traded at {}, {} {} from {}'s order {}", update.qty, update.price,
                                       who(truth, trade->incoming_client), update.side == Side::buy ? "buying" : "selling",
                                       who(truth, trade->resting_client), trade->resting_order_id);
                    traded.insert(trade->resting_order_id);
                } else {
                    said = std::format("trade: {} traded at {}", update.qty, update.price);
                }
                break;
            case UpdateType::cancel:
                said = traded.contains(update.order_id)
                           ? std::format("order {} removed: the trade filled it", update.order_id)
                           : std::format("order {} removed: its client cancelled it", update.order_id);
                break;
            case UpdateType::modify:
                said = std::format("order {} now {}: the trade took part of it", update.order_id, update.qty);
                break;
            default:
                said = std::format("update of type {}", static_cast<int>(update.type));
        }
        shown.updates.emplace_back(u, std::move(said));
    }
    for (std::size_t i = *index + 1; i < truth.steps.size(); ++i) {
        const RequestFields later = decode(truth.steps[i].request);
        if (later.client == request.client && later.ticker == request.ticker) {
            shown.next = std::format("Then, request {}: {}", i + 1, describe_request(truth, truth.steps[i]));
            break;
        }
    }
    return shown;
}

std::optional<std::uint64_t> nearest_request(const SessionTruth& truth, std::uint64_t at, Way way) {
    const auto& steps = truth.steps;
    const auto here = step_of(truth, at);
    if (way == Way::forward) {
        // The first request starting after the one `at` is in (or after `at`, if none made it).
        const std::uint64_t past = here ? steps[*here].first_update + steps[*here].update_count : at + 1;
        for (const ReplayStep& step : steps) {
            if (step.update_count > 0 && step.first_update >= past) {
                return step.first_update;
            }
        }
        return std::nullopt;
    }
    // Back: the last request starting before `at`: the start of the one `at` is in, if `at` is past
    // its start, or else the one before.
    for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
        if (step->update_count > 0 && step->first_update < at) {
            return step->first_update;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> nearest_trade(std::span<const MarketUpdate> updates, std::uint32_t ticker, std::uint64_t at,
                                           Way way, const SessionTruth* slicer) {
    const auto wanted = [&](std::uint64_t seq) {
        const MarketUpdate& update = updates[seq - 1];
        if (update.type != UpdateType::trade || update.ticker != ticker) {
            return false;
        }
        if (slicer == nullptr) {
            return true;
        }
        const TradeTruth* trade = slicer->truth.trade_at(seq);
        return trade != nullptr && (slicer->slicers.contains(trade->incoming_client) || slicer->slicers.contains(trade->resting_client));
    };
    if (way == Way::forward) {
        for (std::uint64_t seq = at + 1; seq <= updates.size(); ++seq) {
            if (wanted(seq)) {
                return seq;
            }
        }
    } else {
        for (std::uint64_t seq = std::min<std::uint64_t>(at, updates.size() + 1); seq-- > 1;) {
            if (wanted(seq)) {
                return seq;
            }
        }
    }
    return std::nullopt;
}

}  // namespace fillscope
