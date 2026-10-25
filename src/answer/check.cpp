#include "answer/check.h"

#include <cctype>
#include <format>
#include <optional>
#include <variant>
#include <vector>

namespace fillscope {
namespace {

// "the maker" becomes "The maker", to start a sentence.
std::string capitalized(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    }
    return text;
}

std::string_view side_word(Side side) { return side == Side::buy ? "buy" : "sell"; }

std::string best_text(Side side, const std::optional<PriceQty>& best) {
    const std::string_view name = side == Side::buy ? "bid" : "ask";
    if (!best) {
        return std::format("the {} side is empty", side_word(side));
    }
    return std::format("the best {} is {}, for {}", name, best->price, best->qty);
}

std::string trades_text(const std::vector<TradeSeen>& trades) {
    if (trades.empty()) {
        return "no trade";
    }
    std::string text;
    for (const TradeSeen& trade : trades) {
        text += std::format("{}{} at {} (an incoming {} crossing)", text.empty() ? "" : ", then ", trade.qty, trade.price,
                            side_word(trade.aggressor));
    }
    return text;
}

std::string_view response_name(ResponseType type) {
    switch (type) {
    case ResponseType::accepted: return "accepted";
    case ResponseType::canceled: return "canceled";
    case ResponseType::filled: return "filled";
    case ResponseType::cancel_rejected: return "cancel rejected";
    case ResponseType::rejected: return "rejected";
    }
    return "unknown";
}

ResponseType wire_type(ResponseKind kind) {
    switch (kind) {
    case ResponseKind::accepted: return ResponseType::accepted;
    case ResponseKind::filled: return ResponseType::filled;
    case ResponseKind::canceled: return ResponseType::canceled;
    case ResponseKind::cancel_rejected: return ResponseType::cancel_rejected;
    }
    return ResponseType::rejected;
}

ClaimResult check_best(const BestClaim& claim, const StepState& state) {
    const auto levels = state.book.levels(claim.side);
    std::optional<PriceQty> actual;
    if (!levels.empty()) {
        actual = PriceQty{.price = levels.front().price, .qty = levels.front().qty};
    }
    ClaimResult result{.says = capitalized(best_text(claim.side, claim.best)) + ".", .holds = actual == claim.best, .actually = {}};
    if (!result.holds) {
        result.actually = best_text(claim.side, actual);
    }
    return result;
}

ClaimResult check_trades(const TradesClaim& claim, const StepState& state) {
    std::vector<TradeSeen> actual;
    for (const MarketUpdate& update : state.updates) {
        if (update.type == UpdateType::trade) {
            // A trade's side is the incoming order's, the one that crossed (the back end's
            // MEOrderBook::match).
            actual.push_back({.price = update.price, .qty = update.qty, .aggressor = update.side});
        }
    }
    ClaimResult result{.says = claim.trades.empty() ? std::string{"This step makes no trade."}
                                                    : std::format("This step trades {}.", trades_text(claim.trades)),
                       .holds = actual == claim.trades,
                       .actually = {}};
    if (!result.holds) {
        result.actually = std::format("it makes {}", trades_text(actual));
    }
    return result;
}

ClaimResult check_resting(const RestingClaim& claim, const StepState& state, const Scenario& scenario) {
    const std::string order = std::format("{}'s order {}", role_of(scenario, claim.client), claim.id);
    ClaimResult result{.says = claim.qty ? std::format("{} rests with {}.", capitalized(order), *claim.qty)
                                         : std::format("{} doesn't rest in the book.", capitalized(order)),
                       .holds = false,
                       .actually = {}};
    const auto market_id = state.market_ids.find({claim.client, claim.id});
    std::optional<std::uint32_t> actual;
    if (market_id != state.market_ids.end()) {
        if (const Order* resting = state.book.find(market_id->second)) {
            actual = resting->qty;
        }
    }
    result.holds = actual == claim.qty;
    if (!result.holds) {
        if (market_id == state.market_ids.end()) {
            result.actually = "the exchange never accepted that order";
        } else {
            result.actually = actual ? std::format("it rests with {}", *actual) : std::string{"it doesn't rest"};
        }
    }
    return result;
}

ClaimResult check_told(const ToldClaim& claim, const StepState& state, const Scenario& scenario) {
    std::string says = std::format("{} is told order {} ", capitalized(role_of(scenario, claim.client)), claim.id);
    switch (claim.kind) {
    case ResponseKind::accepted: says += "was accepted"; break;
    case ResponseKind::filled: says += "filled"; break;
    case ResponseKind::canceled: says += "was canceled"; break;
    case ResponseKind::cancel_rejected: says += "can't be canceled"; break;
    }
    if (claim.qty) {
        says += std::format(" {}", *claim.qty);
    }
    if (claim.price) {
        says += std::format(" at {}", *claim.price);
    }
    if (claim.leaves) {
        says += std::format(", with {} left", *claim.leaves);
    }
    ClaimResult result{.says = says + ".", .holds = false, .actually = {}};
    std::string told;
    for (const ClientResponse& response : state.responses) {
        if (response.client != claim.client || response.client_order_id != claim.id) {
            continue;
        }
        told += std::format("{}{}", told.empty() ? "" : "; ", describe(response));
        result.holds = result.holds ||
                       (response.type == wire_type(claim.kind) && (!claim.price || response.price == *claim.price) &&
                        (!claim.qty || response.exec_qty == *claim.qty) &&
                        (!claim.leaves || response.leaves_qty == *claim.leaves));
    }
    if (!result.holds) {
        result.actually = told.empty() ? std::format("it was told nothing about order {} in this step", claim.id)
                                       : std::format("it was told: {}", told);
    }
    return result;
}

}  // namespace

std::string role_of(const Scenario& scenario, std::uint32_t client) {
    const auto found = scenario.clients.find(client);
    return found != scenario.clients.end() ? found->second : std::format("client {}", client);
}

ClaimResult check_claim(const Claim& claim, const StepState& state, const Scenario& scenario) {
    // One function per kind of claim; the overloads are chosen by the variant's alternative.
    struct Checker {
        const StepState& state;
        const Scenario& scenario;
        ClaimResult operator()(const BestClaim& c) const { return check_best(c, state); }
        ClaimResult operator()(const TradesClaim& c) const { return check_trades(c, state); }
        ClaimResult operator()(const RestingClaim& c) const { return check_resting(c, state, scenario); }
        ClaimResult operator()(const ToldClaim& c) const { return check_told(c, state, scenario); }
    };
    return std::visit(Checker{state, scenario}, claim);
}

std::string describe(const Request& request, const Scenario& scenario) {
    const std::string who = capitalized(role_of(scenario, request.client));
    if (const auto* order = std::get_if<NewOrder>(&request.action)) {
        return std::format("{} sends order {}: {} {} at {}.", who, order->id, side_word(order->side), order->qty, order->price);
    }
    return std::format("{} cancels its order {}.", who, std::get<Cancel>(request.action).id);
}

std::string describe(const ClientResponse& r) {
    switch (r.type) {
    case ResponseType::accepted:
        return std::format("order {} accepted: {} {} at {}", r.client_order_id, side_word(r.side), r.leaves_qty, r.price);
    case ResponseType::filled:
        return std::format("order {} filled {} at {}, {} left", r.client_order_id, r.exec_qty, r.price, r.leaves_qty);
    case ResponseType::canceled:
        return std::format("order {} canceled, {} unfilled", r.client_order_id, r.leaves_qty);
    case ResponseType::cancel_rejected:
        return std::format("cancel of order {} rejected: it doesn't rest", r.client_order_id);
    case ResponseType::rejected:
        return std::format("order {} {}", r.client_order_id, response_name(r.type));
    }
    return std::format("order {}: response {}", r.client_order_id, static_cast<int>(r.type));
}

}  // namespace fillscope
