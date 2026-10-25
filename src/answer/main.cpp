// fillscope-answer: makes an answer for the viewer from a scenario the assistant wrote.
//
//   fillscope-answer SCENARIO [--answer FILE]
//
// Plays the scenario's requests through the back end's matcher (exchange_replay) and checks every
// claim it makes against the replay. If all hold, writes the answer for the viewer, by default
// .fillscope/answer.json, with the replay's files beside it in .fillscope/answer/. If any is false,
// or a request can't be played, says why and writes nothing. Exits 0 when written, 1 when refused,
// 2 for a bad command line.
#include "answer/answer.h"
#include "answer/scenario.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kUsage = "usage: fillscope-answer SCENARIO [--answer FILE]";

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> scenario_file;
    std::filesystem::path answer_file{".fillscope/answer.json"};
    bool answer_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--answer" && i + 1 < argc && !answer_given) {
            answer_file = argv[++i];
            answer_given = true;
        } else if (!arg.starts_with("--") && !scenario_file) {
            scenario_file = arg;
        } else {
            std::cerr << kUsage << "\n";
            return 2;
        }
    }
    if (!scenario_file) {
        std::cerr << kUsage << "\n";
        return 2;
    }
    std::ifstream in{*scenario_file};
    if (!in) {
        std::cerr << "fillscope-answer: can't open " << scenario_file->string() << "\n";
        return 1;
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        std::cerr << "fillscope-answer: " << scenario_file->string() << " isn't valid JSON\n";
        return 1;
    }
    const auto scenario = fillscope::read_scenario(json);
    if (!scenario) {
        std::cerr << "fillscope-answer: " << scenario.error() << "\n";
        return 1;
    }
    const fillscope::AnswerPaths paths{.answer_file = answer_file,
                                       .work = answer_file.parent_path() / "answer",
                                       .exchange_replay = FILLSCOPE_EXCHANGE_REPLAY};
    if (auto made = fillscope::make_answer(*scenario, json, paths); !made) {
        std::cerr << "fillscope-answer: " << made.error();
        if (!made.error().ends_with('\n')) {
            std::cerr << "\n";
        }
        return 1;
    }
    std::size_t claims = 0;
    for (const auto& step : scenario->steps) {
        claims += step.claims.size();
    }
    std::cout << "fillscope-answer: " << scenario->steps.size() << " steps, " << claims << " claims, all true; written to "
              << answer_file.string() << "\n";
    return 0;
}
