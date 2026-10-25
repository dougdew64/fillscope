// An answer's scenario: the requests the assistant's answer plays through the back end's matcher,
// what it says about each, and the claims it makes, which are checked against what the matcher
// really did (answer/check.h). Read from JSON (docs/ARCHITECTURE.md, *Answers*, has the format).
//
// Only the requests drive anything: the books, trades and responses an answer shows are the
// replay's. So a scenario holds only what Ghosh's exchange can be asked to do, and anything it
// can't (a market order, a modify) is refused when read, saying why, rather than played as
// something else.
#pragma once

#include "feed/market_update.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fillscope {

// The back end's limits on a request (backend/Chapter12/common/types.h, ME_MAX_*), which its
// recording refuses past: a scenario is refused before that, in the domain's words.
inline constexpr std::uint32_t kMaxClients = 256;
inline constexpr std::uint32_t kMaxTickers = 8;
inline constexpr std::uint64_t kMaxOrderIds = 1024 * 1024;  // a client's order ids are below this

// A new limit order: every order in Ghosh's exchange has a price, and what doesn't trade rests.
struct NewOrder {
    std::uint64_t id = 0;  // the client's own id for it, unique to that client in a scenario
    Side side = Side::none;
    std::int64_t price = 0;
    std::uint32_t qty = 0;

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

// A cancel of one of the client's orders, by the client's id for it. One the client never sent,
// or that no longer rests, is allowed: the exchange rejects it, which a scenario may want to show.
struct Cancel {
    std::uint64_t id = 0;

    friend bool operator==(const Cancel&, const Cancel&) = default;
};

struct Request {
    std::uint32_t client = 0;
    std::variant<NewOrder, Cancel> action;

    friend bool operator==(const Request&, const Request&) = default;
};

// ---- Claims: facts a step states, each checked against the replay ----

struct PriceQty {
    std::int64_t price = 0;
    std::uint64_t qty = 0;  // the total resting at that price

    friend bool operator==(const PriceQty&, const PriceQty&) = default;
};

// The best bid or ask after the step, or that side empty (nullopt).
struct BestClaim {
    Side side = Side::none;
    std::optional<PriceQty> best;

    friend bool operator==(const BestClaim&, const BestClaim&) = default;
};

struct TradeSeen {
    std::int64_t price = 0;
    std::uint32_t qty = 0;
    Side aggressor = Side::none;  // the side of the incoming order that caused it

    friend bool operator==(const TradeSeen&, const TradeSeen&) = default;
};

// Exactly the step's trades, in order: one left out is as false as one made up.
struct TradesClaim {
    std::vector<TradeSeen> trades;

    friend bool operator==(const TradesClaim&, const TradesClaim&) = default;
};

// A client's order, by its id, rests after the step with this quantity, or doesn't rest (nullopt).
struct RestingClaim {
    std::uint32_t client = 0;
    std::uint64_t id = 0;
    std::optional<std::uint32_t> qty;

    friend bool operator==(const RestingClaim&, const RestingClaim&) = default;
};

enum class ResponseKind { accepted, filled, canceled, cancel_rejected };

// The client was sent this response in the step, about its order `id`. The numbers given are
// checked; those left out aren't (a cancel's response, for one, carries no price worth stating).
struct ToldClaim {
    std::uint32_t client = 0;
    ResponseKind kind = ResponseKind::accepted;
    std::uint64_t id = 0;
    std::optional<std::int64_t> price;
    std::optional<std::uint32_t> qty;     // a fill's quantity
    std::optional<std::uint32_t> leaves;  // what remains of the order after it

    friend bool operator==(const ToldClaim&, const ToldClaim&) = default;
};

using Claim = std::variant<BestClaim, TradesClaim, RestingClaim, ToldClaim>;

struct ScenarioStep {
    Request request;
    std::string say;
    std::vector<Claim> claims;
};

struct Scenario {
    std::string question;
    std::uint32_t ticker = 0;
    std::map<std::uint32_t, std::string> clients;  // each client's role in the answer: "the maker"
    std::string intro;
    std::vector<ScenarioStep> steps;
};

// The scenario `json` describes, or why it can't be played: the first problem found, naming the
// step and field. Unknown fields are refused too, so a misspelt claim isn't silently unchecked.
[[nodiscard]] std::expected<Scenario, std::string> read_scenario(const nlohmann::json& json);

// The same, from JSON text, which may not parse.
[[nodiscard]] std::expected<Scenario, std::string> read_scenario(std::string_view text);

[[nodiscard]] std::string_view to_string(ResponseKind kind) noexcept;

}  // namespace fillscope
