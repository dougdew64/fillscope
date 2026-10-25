// Answers made by hand for the viewer's tests: an answer file and the replay's two files beside it,
// as the answer tool (src/answer/) writes them, but from updates and steps given here, with no
// exchange_replay. The steps and updates must be what the back end would have made of the requests;
// the answer tool's own tests, on real replays, check that it does.
#pragma once

#include "answer/answer.h"
#include "answer/scenario.h"
#include "answer/wire.h"
#include "feed/market_update.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fillscope::test {

// Writes a market data file as exchange_replay does: "LLMDAT01", the recording's path, the updates.
inline void write_market_data(const std::filesystem::path& file, const std::string& recording,
                              const std::vector<MarketUpdate>& updates) {
    std::ofstream out{file, std::ios::binary};
    const auto length = static_cast<std::uint32_t>(recording.size());
    out << "LLMDAT01";
    out.write(reinterpret_cast<const char*>(&length), sizeof length);
    out << recording;
    for (const MarketUpdate& update : updates) {
        const auto bytes = encode_update(update);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

// Writes the answer for `scenario_json` to `answer_file`, with its replay's files in the answer
// file's directory under answer/, as the tool would. Throws if the scenario can't be read or the
// steps don't fit it: a fixture that is wrong should stop the test, not show as a viewer's fault.
inline void write_answer(const std::filesystem::path& answer_file, const nlohmann::json& scenario_json,
                         const std::vector<MarketUpdate>& updates, const std::vector<ReplayStep>& steps) {
    const auto scenario = read_scenario(scenario_json);
    if (!scenario) {
        throw std::runtime_error{"fixture scenario: " + scenario.error()};
    }
    const std::filesystem::path work = answer_file.parent_path() / "answer";
    std::filesystem::create_directories(work);
    write_market_data(work / "market.dat", "scenario.rec", updates);
    StepsFile steps_file{.recording = "scenario.rec", .steps = steps};
    for (std::size_t i = 0; i < steps_file.steps.size() && i < scenario->steps.size(); ++i) {
        steps_file.steps[i].request = encode_request(scenario->steps[i].request, scenario->ticker);
    }
    const std::vector<std::byte> bytes = encode_steps_file(steps_file);
    std::ofstream{work / "steps.dat", std::ios::binary}.write(reinterpret_cast<const char*>(bytes.data()),
                                                              static_cast<std::streamsize>(bytes.size()));
    const auto checked = check_scenario(*scenario, updates, steps_file);
    if (!checked) {
        throw std::runtime_error{"fixture steps: " + checked.error()};
    }
    const ReplayFiles replay{.market_data = work / "market.dat",
                             .market_data_fingerprint = *fingerprint_file(work / "market.dat"),
                             .steps = work / "steps.dat",
                             .steps_fingerprint = *fingerprint_file(work / "steps.dat")};
    // Written as the tool writes it, by renaming a new file over the old, so a watcher sees a new one.
    const std::filesystem::path temporary = answer_file.string() + ".tmp";
    std::ofstream{temporary} << answer_json(*scenario, scenario_json, *checked, replay).dump(1);
    std::filesystem::rename(temporary, answer_file);
}

// The maker and the taker, on ticker 0: the maker bids 10 at 99 (its order 10, the exchange's 1),
// offers 10 at 101 (11, the exchange's 2); the taker buys 4 at 101 (30, the exchange's 3), asking
// first what trades, which fills 4 of the maker's offer. `question` names the answer, so two can be
// told apart. Its claims are all true; `false_claim` makes the last one false.
inline nlohmann::json maker_and_taker_scenario(const std::string& question, bool false_claim = false) {
    return nlohmann::json::parse(std::string{R"({
      "question": ")"} + question + R"(",
      "clients": { "1": "the maker", "3": "the taker" },
      "intro": "A maker posts orders that wait; a taker trades with them at once.",
      "steps": [
        { "request": { "client": 1, "new": { "id": 10, "side": "buy", "price": 99, "qty": 10 } },
          "say": "The maker bids 10 at 99.",
          "claims": [ { "bestBid": { "price": 99, "qty": 10 } } ] },
        { "request": { "client": 1, "new": { "id": 11, "side": "sell", "price": 101, "qty": 10 } },
          "say": "And offers 10 at 101." },
        { "request": { "client": 3, "new": { "id": 30, "side": "buy", "price": 101, "qty": 4 } },
          "ask": "The taker buys 4 at 101: what trades?",
          "say": "It trades 4 at 101, the resting order's price.",
          "claims": [ { "trades": [ { "price": 101, "qty": 4, "aggressor": "buy" } ] },
                      { "bestAsk": { "price": 101, "qty": )" + std::string{false_claim ? "10" : "6"} + R"( } } ] }
      ]
    })");
}

inline void write_maker_and_taker(const std::filesystem::path& answer_file, const std::string& question,
                                  bool false_claim = false) {
    const auto add = [](std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty) {
        return MarketUpdate{.seq = seq, .type = UpdateType::add, .order_id = id, .ticker = 0, .side = side, .price = price,
                            .qty = qty, .priority = 1};
    };
    const std::vector<MarketUpdate> updates = {
        add(1, 1, Side::buy, 99, 10), add(2, 2, Side::sell, 101, 10),
        {.seq = 3, .type = UpdateType::trade, .order_id = 0, .ticker = 0, .side = Side::buy, .price = 101, .qty = 4, .priority = 0},
        {.seq = 4, .type = UpdateType::modify, .order_id = 2, .ticker = 0, .side = Side::sell, .price = 101, .qty = 6, .priority = 1}};
    const auto response = [](ResponseType type, std::uint32_t client, std::uint64_t id, std::uint64_t market_id, Side side,
                             std::int64_t price, std::uint32_t exec, std::uint32_t leaves) {
        return ClientResponse{.type = type, .client = client, .ticker = 0, .client_order_id = id, .market_order_id = market_id,
                              .side = side, .price = price, .exec_qty = exec, .leaves_qty = leaves};
    };
    const std::vector<ReplayStep> steps = {
        {.request = {}, .first_update = 1, .update_count = 1,
         .responses = {response(ResponseType::accepted, 1, 10, 1, Side::buy, 99, 0, 10)}},
        {.request = {}, .first_update = 2, .update_count = 1,
         .responses = {response(ResponseType::accepted, 1, 11, 2, Side::sell, 101, 0, 10)}},
        {.request = {}, .first_update = 3, .update_count = 2,
         .responses = {response(ResponseType::accepted, 3, 30, 3, Side::buy, 101, 0, 4),
                       response(ResponseType::filled, 3, 30, 3, Side::buy, 101, 4, 0),
                       response(ResponseType::filled, 1, 11, 2, Side::sell, 101, 4, 6)}}};
    write_answer(answer_file, maker_and_taker_scenario(question, false_claim), updates, steps);
}

}  // namespace fillscope::test
