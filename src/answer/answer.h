// Making an answer from a scenario (answer/scenario.h): its requests written as a recording,
// replayed through the back end's matcher by exchange_replay, and every claim checked against the
// replay; the result written for the viewer to show, step by step.
#pragma once

#include "answer/check.h"
#include "answer/scenario.h"
#include "answer/wire.h"
#include "feed/market_update.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fillscope {

// What the replay of a scenario did, and every claim checked against it.
struct CheckedStep {
    std::uint64_t first_update = 0;
    std::uint32_t update_count = 0;
    std::vector<ClientResponse> responses;
    std::vector<ClaimResult> claims;
};

// Checks `scenario`'s claims against its replay: the market updates it gave, and its steps, one for
// each of the scenario's. Returns why the replay doesn't fit the scenario if it doesn't (a step
// that isn't the scenario's request, updates that don't follow on), or every step's results, true
// claims and false alike.
[[nodiscard]] std::expected<std::vector<CheckedStep>, std::string> check_scenario(const Scenario& scenario,
                                                                                   const std::vector<MarketUpdate>& updates,
                                                                                   const StepsFile& steps);

// The false claims in `checked`, one line each, naming the step, what was claimed and what is true;
// empty if every claim holds.
[[nodiscard]] std::string false_claims(const std::vector<CheckedStep>& checked);

// A file's fingerprint, to tell later whether it has changed: FNV-1a over its bytes, as text. For
// telling a stale answer from a current one, not for security.
[[nodiscard]] std::expected<std::string, std::string> fingerprint_file(const std::filesystem::path& path);

struct AnswerPaths {
    std::filesystem::path answer_file;      // what the viewer reads: by default .fillscope/answer.json
    std::filesystem::path work;             // the recording, the replay's files and its log
    std::filesystem::path exchange_replay;  // the back end's program
};

// Plays `scenario` (read from `scenario_json`) and writes the answer. Returns why not, if a step
// can't be played or a claim is false: a false claim is never shown, so the answer file is left as
// it was.
[[nodiscard]] std::expected<void, std::string> make_answer(const Scenario& scenario, const nlohmann::json& scenario_json,
                                                           const AnswerPaths& paths);

// The answer file's contents, for a scenario whose every claim holds. The scenario goes in as it was
// given, so the viewer can read it with read_scenario and check the claims against its own books.
[[nodiscard]] nlohmann::ordered_json answer_json(const Scenario& scenario, const nlohmann::json& scenario_json,
                                                 const std::vector<CheckedStep>& checked,
                                                 const std::filesystem::path& market_data_file,
                                                 const std::string& market_data_fingerprint);

}  // namespace fillscope
