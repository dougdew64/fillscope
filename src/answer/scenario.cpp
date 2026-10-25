#include "answer/scenario.h"

#include "common/parse.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace fillscope {
namespace {

using Json = nlohmann::json;

// Generous bounds that keep every value well inside the back end's types and away from their
// "invalid" markers (the largest value of each).
constexpr std::int64_t kMaxPrice = 1'000'000'000;
constexpr std::int64_t kMaxQty = 1'000'000'000;

std::unexpected<std::string> refuse(std::string_view where, std::string_view why) {
    return std::unexpected{std::format("{}: {}", where, why)};
}

// The first of `object`'s fields not in `allowed`, if any, so a misspelt field is refused rather
// than ignored.
std::optional<std::string> unknown_field(const Json& object, std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, value] : object.items()) {
        bool known = false;
        for (const std::string_view name : allowed) {
            known = known || key == name;
        }
        if (!known) {
            return key;
        }
    }
    return std::nullopt;
}

std::expected<void, std::string> only_fields(const Json& object, std::string_view where,
                                             std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) {
        return refuse(where, "must be an object");
    }
    if (const auto unknown = unknown_field(object, allowed)) {
        std::string names;
        for (const std::string_view name : allowed) {
            names += std::format("{}{}", names.empty() ? "" : ", ", name);
        }
        return refuse(where, std::format("has a field '{}', and the fields it may have are: {}", *unknown, names));
    }
    return {};
}

// The first of `results` that failed, and why, if any did: for a field's several parts, read in
// turn. A fold expression over the parameter pack (C++17) visits each result in order.
template <typename... Ts>
std::optional<std::string> first_error(const std::expected<Ts, std::string>&... results) {
    std::optional<std::string> found;
    const auto take = [&found](const auto& result) {
        if (!found && !result) {
            found = result.error();
        }
    };
    (take(results), ...);
    return found;
}

// A whole number from `min` to `max`. JSON numbers with a fraction, and ones written as text, are
// refused: 101.5 is no price in an exchange whose prices are whole ticks.
std::expected<std::int64_t, std::string> integer(const Json& object, std::string_view field, std::string_view where,
                                                 std::int64_t min, std::int64_t max) {
    const auto found = object.find(field);
    if (found == object.end()) {
        return refuse(where, std::format("needs '{}'", field));
    }
    const std::string at = std::format("{}.{}", where, field);
    if (!found->is_number_integer()) {
        return refuse(at, std::format("must be a whole number, not {}", found->dump()));
    }
    if (found->is_number_unsigned() && found->get<std::uint64_t>() > static_cast<std::uint64_t>(max)) {
        return refuse(at, std::format("is {}, and may be at most {}", found->dump(), max));
    }
    const auto value = found->get<std::int64_t>();
    if (value < min || value > max) {
        return refuse(at, std::format("is {}, and must be from {} to {}", value, min, max));
    }
    return value;
}

std::expected<std::string, std::string> text(const Json& object, std::string_view field, std::string_view where,
                                             bool required) {
    const auto found = object.find(field);
    if (found == object.end()) {
        if (required) {
            return refuse(where, std::format("needs '{}'", field));
        }
        return std::string{};
    }
    if (!found->is_string() || (required && found->get<std::string>().empty())) {
        return refuse(std::format("{}.{}", where, field), "must be text, and not empty");
    }
    return found->get<std::string>();
}

std::expected<Side, std::string> side(const Json& object, std::string_view field, std::string_view where) {
    const auto found = object.find(field);
    if (found == object.end()) {
        return refuse(where, std::format("needs '{}'", field));
    }
    if (*found == "buy") {
        return Side::buy;
    }
    if (*found == "sell") {
        return Side::sell;
    }
    return refuse(std::format("{}.{}", where, field), std::format("must be \"buy\" or \"sell\", not {}", found->dump()));
}

// The client a request or a claim names, which the scenario's `clients` must give a role.
std::expected<std::uint32_t, std::string> client(const Json& object, std::string_view where, const Scenario& scenario) {
    auto value = integer(object, "client", where, 0, kMaxClients - 1);
    if (!value) {
        return std::unexpected{value.error()};
    }
    const auto id = static_cast<std::uint32_t>(*value);
    if (!scenario.clients.contains(id)) {
        return refuse(where, std::format("names client {}, which 'clients' gives no role", id));
    }
    return id;
}

std::expected<std::uint64_t, std::string> order_id(const Json& object, std::string_view where) {
    auto value = integer(object, "id", where, 0, static_cast<std::int64_t>(kMaxOrderIds) - 1);
    if (!value) {
        return std::unexpected{value.error()};
    }
    return static_cast<std::uint64_t>(*value);
}

std::expected<Request, std::string> read_request(const Json& json, std::string_view where, const Scenario& scenario,
                                                 std::set<std::pair<std::uint32_t, std::uint64_t>>& ids_used) {
    // What Ghosh's exchange can't be asked to do, refused in the domain's words.
    if (json.is_object() && json.contains("market")) {
        return refuse(where, "Ghosh's exchange has no market orders: every order has a price, so a taker is a limit "
                             "order priced to cross the best price on the other side");
    }
    if (json.is_object() && (json.contains("modify") || json.contains("replace"))) {
        return refuse(where, "a client can't modify an order in Ghosh's exchange: it cancels it and sends a new one");
    }
    if (auto fields = only_fields(json, where, {"client", "new", "cancel"}); !fields) {
        return std::unexpected{fields.error()};
    }
    if (json.contains("new") == json.contains("cancel")) {
        return refuse(where, "needs exactly one of 'new' and 'cancel'");
    }
    auto who = client(json, where, scenario);
    if (!who) {
        return std::unexpected{who.error()};
    }
    Request request{.client = *who, .action = Cancel{}};
    if (json.contains("cancel")) {
        const std::string at = std::format("{}.cancel", where);
        const Json& cancel = json["cancel"];
        if (auto fields = only_fields(cancel, at, {"id"}); !fields) {
            return std::unexpected{fields.error()};
        }
        auto id = order_id(cancel, at);
        if (!id) {
            return std::unexpected{id.error()};
        }
        request.action = Cancel{.id = *id};
        return request;
    }
    const std::string at = std::format("{}.new", where);
    const Json& order = json["new"];
    if (auto fields = only_fields(order, at, {"id", "side", "price", "qty"}); !fields) {
        return std::unexpected{fields.error()};
    }
    auto id = order_id(order, at);
    if (!id) {
        return std::unexpected{id.error()};
    }
    // The exchange keys a client's orders by its id, and a second order under a used id would
    // replace the first's entry there: a scenario uses each id once.
    if (!ids_used.insert({*who, *id}).second) {
        return refuse(at, std::format("uses order id {} again for client {}: each new order needs an id of its own", *id, *who));
    }
    auto order_side = side(order, "side", at);
    auto price = integer(order, "price", at, 1, kMaxPrice);
    auto qty = integer(order, "qty", at, 1, kMaxQty);
    if (const auto problem = first_error(order_side, price, qty)) {
        return std::unexpected{*problem};
    }
    request.action = NewOrder{.id = *id, .side = *order_side, .price = *price, .qty = static_cast<std::uint32_t>(*qty)};
    return request;
}

std::expected<Claim, std::string> read_claim(const Json& json, std::string_view where, const Scenario& scenario) {
    if (!json.is_object() || json.size() != 1) {
        return refuse(where, "must be an object with one field: bestBid, bestAsk, trades, resting or told");
    }
    const auto& [kind, value] = *json.items().begin();
    const std::string at = std::format("{}.{}", where, kind);
    if (kind == "bestBid" || kind == "bestAsk") {
        BestClaim claim{.side = kind == "bestBid" ? Side::buy : Side::sell, .best = std::nullopt};
        if (value.is_null()) {
            return claim;  // that side is empty
        }
        if (auto fields = only_fields(value, at, {"price", "qty"}); !fields) {
            return std::unexpected{fields.error()};
        }
        auto price = integer(value, "price", at, 1, kMaxPrice);
        auto qty = integer(value, "qty", at, 1, std::numeric_limits<std::int64_t>::max());
        if (const auto problem = first_error(price, qty)) {
            return std::unexpected{*problem};
        }
        claim.best = PriceQty{.price = *price, .qty = static_cast<std::uint64_t>(*qty)};
        return claim;
    }
    if (kind == "trades") {
        if (!value.is_array()) {
            return refuse(at, "must be a list of trades, empty for none");
        }
        TradesClaim claim;
        for (std::size_t i = 0; i < value.size(); ++i) {
            const std::string trade_at = std::format("{}[{}]", at, i);
            if (auto fields = only_fields(value[i], trade_at, {"price", "qty", "aggressor"}); !fields) {
                return std::unexpected{fields.error()};
            }
            auto price = integer(value[i], "price", trade_at, 1, kMaxPrice);
            auto qty = integer(value[i], "qty", trade_at, 1, kMaxQty);
            auto aggressor = side(value[i], "aggressor", trade_at);
            if (const auto problem = first_error(price, qty, aggressor)) {
                return std::unexpected{*problem};
            }
            claim.trades.push_back({.price = *price, .qty = static_cast<std::uint32_t>(*qty), .aggressor = *aggressor});
        }
        return claim;
    }
    if (kind == "resting") {
        if (auto fields = only_fields(value, at, {"client", "id", "qty"}); !fields) {
            return std::unexpected{fields.error()};
        }
        auto who = client(value, at, scenario);
        auto id = order_id(value, at);
        if (const auto problem = first_error(who, id)) {
            return std::unexpected{*problem};
        }
        RestingClaim claim{.client = *who, .id = *id, .qty = std::nullopt};
        if (!value.contains("qty")) {
            return refuse(at, "needs 'qty': the quantity it rests with, or null if it doesn't rest");
        }
        if (!value["qty"].is_null()) {
            auto qty = integer(value, "qty", at, 1, kMaxQty);
            if (!qty) {
                return std::unexpected{qty.error()};
            }
            claim.qty = static_cast<std::uint32_t>(*qty);
        }
        return claim;
    }
    if (kind == "told") {
        if (auto fields = only_fields(value, at, {"client", "accepted", "filled", "canceled", "cancelRejected"}); !fields) {
            return std::unexpected{fields.error()};
        }
        auto who = client(value, at, scenario);
        if (!who) {
            return std::unexpected{who.error()};
        }
        constexpr std::pair<std::string_view, ResponseKind> kKinds[] = {{"accepted", ResponseKind::accepted},
                                                                        {"filled", ResponseKind::filled},
                                                                        {"canceled", ResponseKind::canceled},
                                                                        {"cancelRejected", ResponseKind::cancel_rejected}};
        std::optional<ToldClaim> claim;
        for (const auto& [name, response] : kKinds) {
            if (!value.contains(name)) {
                continue;
            }
            if (claim) {
                return refuse(at, "names more than one response: a claim is about one");
            }
            const std::string response_at = std::format("{}.{}", at, name);
            const Json& about = value[std::string{name}];
            if (auto fields = only_fields(about, response_at, {"id", "price", "qty", "leaves"}); !fields) {
                return std::unexpected{fields.error()};
            }
            auto id = order_id(about, response_at);
            if (!id) {
                return std::unexpected{id.error()};
            }
            claim = ToldClaim{.client = *who, .kind = response, .id = *id, .price = {}, .qty = {}, .leaves = {}};
            if (about.contains("price")) {
                auto price = integer(about, "price", response_at, 1, kMaxPrice);
                if (!price) {
                    return std::unexpected{price.error()};
                }
                claim->price = *price;
            }
            for (const auto& [field, target] : {std::pair{"qty", &claim->qty}, std::pair{"leaves", &claim->leaves}}) {
                if (about.contains(field)) {
                    auto number = integer(about, field, response_at, 0, kMaxQty);
                    if (!number) {
                        return std::unexpected{number.error()};
                    }
                    *target = static_cast<std::uint32_t>(*number);
                }
            }
        }
        if (!claim) {
            return refuse(at, "needs one of accepted, filled, canceled and cancelRejected");
        }
        return *claim;
    }
    return refuse(where, std::format("'{}' is no kind of claim: bestBid, bestAsk, trades, resting or told", kind));
}

}  // namespace

std::string_view to_string(ResponseKind kind) noexcept {
    switch (kind) {
    case ResponseKind::accepted: return "accepted";
    case ResponseKind::filled: return "filled";
    case ResponseKind::canceled: return "canceled";
    case ResponseKind::cancel_rejected: return "cancel rejected";
    }
    return "unknown";
}

std::expected<Scenario, std::string> read_scenario(const Json& json) {
    if (auto fields = only_fields(json, "the scenario", {"question", "ticker", "clients", "intro", "steps"}); !fields) {
        return std::unexpected{fields.error()};
    }
    Scenario scenario;
    auto question = text(json, "question", "the scenario", true);
    auto intro = text(json, "intro", "the scenario", false);
    if (const auto problem = first_error(question, intro)) {
        return std::unexpected{*problem};
    }
    scenario.question = *question;
    scenario.intro = *intro;
    if (json.contains("ticker")) {
        auto ticker = integer(json, "ticker", "the scenario", 0, kMaxTickers - 1);
        if (!ticker) {
            return std::unexpected{ticker.error()};
        }
        scenario.ticker = static_cast<std::uint32_t>(*ticker);
    }
    const auto clients = json.find("clients");
    if (clients == json.end() || !clients->is_object() || clients->empty()) {
        return refuse("the scenario", "needs 'clients': each client's number and its role, such as {\"1\": \"the maker\"}");
    }
    for (const auto& [number, role] : clients->items()) {
        // The keys are JSON's text; each must be exactly a client's number.
        const auto id = parse_number<std::uint32_t>(number);
        if (!id || *id >= kMaxClients) {
            return refuse("clients", std::format("'{}' isn't a client number from 0 to {}", number, kMaxClients - 1));
        }
        if (!role.is_string() || role.get<std::string>().empty()) {
            return refuse(std::format("clients.{}", number), "must be the client's role, as text");
        }
        scenario.clients[*id] = role.get<std::string>();
    }
    const auto steps = json.find("steps");
    if (steps == json.end() || !steps->is_array() || steps->empty()) {
        return refuse("the scenario", "needs 'steps', a list of at least one");
    }
    std::set<std::pair<std::uint32_t, std::uint64_t>> ids_used;
    for (std::size_t i = 0; i < steps->size(); ++i) {
        const std::string where = std::format("step {}", i + 1);
        const Json& step = (*steps)[i];
        if (auto fields = only_fields(step, where, {"request", "ask", "say", "claims"}); !fields) {
            return std::unexpected{fields.error()};
        }
        if (!step.contains("request")) {
            return refuse(where, "needs 'request'");
        }
        auto request = read_request(step["request"], std::format("{}, request", where), scenario, ids_used);
        if (!request) {
            return std::unexpected{request.error()};
        }
        auto say = text(step, "say", where, true);
        auto ask = text(step, "ask", where, false);
        if (const auto problem = first_error(say, ask)) {
            return std::unexpected{*problem};
        }
        ScenarioStep read{.request = *request, .say = *say, .claims = {}, .ask = *ask};
        if (step.contains("claims")) {
            if (!step["claims"].is_array()) {
                return refuse(std::format("{}.claims", where), "must be a list");
            }
            for (std::size_t j = 0; j < step["claims"].size(); ++j) {
                auto claim = read_claim(step["claims"][j], std::format("{}, claim {}", where, j + 1), scenario);
                if (!claim) {
                    return std::unexpected{claim.error()};
                }
                read.claims.push_back(std::move(*claim));
            }
        }
        scenario.steps.push_back(std::move(read));
    }
    return scenario;
}

std::expected<Scenario, std::string> read_scenario(std::string_view text) {
    // The non-throwing parse: a value that is "discarded" says the text isn't JSON.
    const Json json = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return std::unexpected{"the scenario isn't valid JSON"};
    }
    return read_scenario(json);
}

}  // namespace fillscope
