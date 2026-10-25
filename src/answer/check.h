// Checking an answer's claims (answer/scenario.h) against what the replay really did, and saying
// each in words, as the viewer's pane shows it.
#pragma once

#include "answer/scenario.h"
#include "answer/wire.h"
#include "feed/book.h"
#include "feed/market_update.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>

namespace fillscope {

// Each client's order, by the client's id for it, to the exchange's id for it: what the market
// data and the book know it by. From the accepted responses, as far as the replay has got.
using MarketIds = std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint64_t>;

// Everything a claim about one step can be checked against.
struct StepState {
    const Book& book;                            // the scenario's ticker, after the step
    std::span<const MarketUpdate> updates;       // the step's own
    std::span<const ClientResponse> responses;   // the step's own, in the order sent
    const MarketIds& market_ids;                 // up to and including the step
};

struct ClaimResult {
    std::string says;      // the claim, as a sentence
    bool holds = false;
    std::string actually;  // what is true instead, when it doesn't hold
};

// The client's role, as the scenario names it ("the maker"), or "client N" if it names none.
[[nodiscard]] std::string role_of(const Scenario& scenario, std::uint32_t client);

[[nodiscard]] ClaimResult check_claim(const Claim& claim, const StepState& state, const Scenario& scenario);

// The step's request, and a response the matcher sent, in words.
[[nodiscard]] std::string describe(const Request& request, const Scenario& scenario);
[[nodiscard]] std::string describe(const ClientResponse& response);

}  // namespace fillscope
