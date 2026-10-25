#include "answer/answer.h"

#include "common/atomic_file.h"
#include "common/fd.h"
#include "feed/book_builder.h"
#include "feed/market_data_file.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fillscope {
namespace {

std::string last_error() { return std::error_code{errno, std::system_category()}.message(); }

// Runs `program` with `args` in `directory`, its output and errors to `log`; returns why it failed if
// it didn't exit 0 (the end of its log says why, as the back end's programs report). fork and exec,
// rather than a shell, so no argument is ever read as shell syntax.
std::expected<void, std::string> run_in(const std::filesystem::path& directory, const std::filesystem::path& log,
                                        const std::filesystem::path& program, const std::vector<std::string>& args) {
    const Fd output{::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
    if (!output) {
        return std::unexpected{std::format("can't create {}: {}", log.string(), last_error())};
    }
    std::vector<std::string> argv_text{program.string()};
    argv_text.insert(argv_text.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : argv_text) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        return std::unexpected{std::format("can't start {}: {}", program.string(), last_error())};
    }
    if (child == 0) {
        // In the child: only async-signal-safe calls until exec, then exit without running the
        // parent's destructors (_exit).
        if (::chdir(directory.c_str()) != 0 || ::dup2(output.get(), STDOUT_FILENO) < 0 ||
            ::dup2(output.get(), STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::unexpected{std::format("can't wait for {}: {}", program.string(), last_error())};
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return {};
    }
    std::ifstream in{log};
    const std::string said{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const std::string how = WIFEXITED(status) ? std::format("exited {}", WEXITSTATUS(status))
                                              : std::format("was stopped by signal {}", WTERMSIG(status));
    return std::unexpected{std::format("{} {}{}{}", program.filename().string(), how, said.empty() ? "" : ": ", said)};
}

}  // namespace

std::expected<std::vector<CheckedStep>, std::string> check_scenario(const Scenario& scenario,
                                                                    const std::vector<MarketUpdate>& updates,
                                                                    const StepsFile& steps) {
    if (steps.steps.size() != scenario.steps.size()) {
        return std::unexpected{
            std::format("the replay has {} steps and the scenario {}", steps.steps.size(), scenario.steps.size())};
    }
    BookBuilder books;
    MarketIds market_ids;
    std::vector<CheckedStep> checked;
    std::uint64_t next_update = 1;
    for (std::size_t i = 0; i < steps.steps.size(); ++i) {
        const ReplayStep& step = steps.steps[i];
        const ScenarioStep& wanted = scenario.steps[i];
        if (step.request != encode_request(wanted.request, scenario.ticker)) {
            return std::unexpected{std::format("the replay's step {} isn't the scenario's request", i + 1)};
        }
        if (step.first_update != next_update || step.update_count > updates.size() - (next_update - 1)) {
            return std::unexpected{std::format("the replay's step {} says its updates start at {}, for {}, and {} of {} "
                                               "were already accounted for",
                                               i + 1, step.first_update, step.update_count, next_update - 1, updates.size())};
        }
        // The replay's updates are numbered 1, 2, 3, ..., so update n is updates[n - 1].
        const std::span<const MarketUpdate> made{updates.data() + (next_update - 1), step.update_count};
        for (const MarketUpdate& update : made) {
            books.on_incremental(update);
        }
        next_update += step.update_count;
        if (books.status() != BookBuilder::Status::live) {
            return std::unexpected{std::format("the books can't follow the replay at step {}: {}", i + 1,
                                               books.recovering_because().value_or("they aren't live"))};
        }
        for (const ClientResponse& response : step.responses) {
            if (response.type == ResponseType::accepted) {
                market_ids[{response.client, response.client_order_id}] = response.market_order_id;
            }
        }
        const StepState state{.book = books.book(scenario.ticker), .updates = made, .responses = step.responses,
                              .market_ids = market_ids};
        CheckedStep result{.first_update = step.first_update, .update_count = step.update_count,
                           .responses = step.responses, .claims = {}};
        for (const Claim& claim : wanted.claims) {
            result.claims.push_back(check_claim(claim, state, scenario));
        }
        checked.push_back(std::move(result));
    }
    if (next_update - 1 != updates.size()) {
        return std::unexpected{std::format("the replay made {} updates, and its steps account for {}", updates.size(),
                                           next_update - 1)};
    }
    return checked;
}

std::string false_claims(const std::vector<CheckedStep>& checked) {
    std::string text;
    for (std::size_t i = 0; i < checked.size(); ++i) {
        for (const ClaimResult& claim : checked[i].claims) {
            if (!claim.holds) {
                text += std::format("step {}: claims \"{}\", but {}\n", i + 1, claim.says, claim.actually);
            }
        }
    }
    return text;
}

std::expected<std::string, std::string> fingerprint_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected{std::format("can't open {}", path.string())};
    }
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325;  // FNV-1a's 64-bit offset basis
    for (std::istreambuf_iterator<char> it{in}, end; it != end; ++it) {
        hash ^= static_cast<unsigned char>(*it);
        hash *= 0x100'0000'01b3;  // its prime
    }
    return std::format("fnv1a64:{:016x}", hash);
}

nlohmann::ordered_json answer_json(const Scenario& scenario, const nlohmann::json& scenario_json,
                                   const std::vector<CheckedStep>& checked, const std::filesystem::path& market_data_file,
                                   const std::string& market_data_fingerprint) {
    nlohmann::ordered_json json;
    json["version"] = 1;
    json["about"] =
        "An answer to the Fillscope viewer's user, from the assistant: a scenario of client requests, played through "
        "the back end's matching engine by exchange_replay, so every book, trade and response is the engine's. Each "
        "step is one request; its claims were checked against the replay, and all hold. 'replay' names the market "
        "data the steps rest on, and its fingerprint, to tell a stale answer from a current one. 'scenario' is the "
        "answer as written.";
    json["question"] = scenario.question;
    json["intro"] = scenario.intro;
    json["ticker"] = scenario.ticker;
    nlohmann::ordered_json clients = nlohmann::ordered_json::object();
    for (const auto& [client, role] : scenario.clients) {
        clients[std::to_string(client)] = role;
    }
    json["clients"] = clients;
    json["replay"] = {{"marketDataFile", std::filesystem::absolute(market_data_file).string()},
                      {"marketDataFingerprint", market_data_fingerprint}};
    nlohmann::ordered_json steps = nlohmann::ordered_json::array();
    for (std::size_t i = 0; i < checked.size(); ++i) {
        const ScenarioStep& step = scenario.steps[i];
        nlohmann::ordered_json responses = nlohmann::ordered_json::array();
        for (const ClientResponse& response : checked[i].responses) {
            responses.push_back({{"client", response.client}, {"to", role_of(scenario, response.client)},
                                 {"says", describe(response)}});
        }
        nlohmann::ordered_json claims = nlohmann::ordered_json::array();
        for (const ClaimResult& claim : checked[i].claims) {
            claims.push_back({{"says", claim.says}, {"holds", claim.holds}});
        }
        steps.push_back({{"number", i + 1},
                         {"client", step.request.client},
                         {"request", describe(step.request, scenario)},
                         {"say", step.say},
                         {"updates", {{"first", checked[i].first_update}, {"count", checked[i].update_count}}},
                         {"responses", responses},
                         {"claims", claims}});
    }
    json["steps"] = steps;
    json["scenario"] = scenario_json;
    return json;
}

std::expected<void, std::string> make_answer(const Scenario& scenario, const nlohmann::json& scenario_json,
                                             const AnswerPaths& paths) {
    std::error_code error;
    std::filesystem::create_directories(paths.work, error);
    if (error) {
        return std::unexpected{std::format("can't create {}: {}", paths.work.string(), error.message())};
    }
    const std::filesystem::path recording = paths.work / "scenario.rec";
    const std::filesystem::path market_data = paths.work / "market.dat";
    const std::filesystem::path steps_file = paths.work / "steps.dat";
    std::vector<Request> requests;
    for (const ScenarioStep& step : scenario.steps) {
        requests.push_back(step.request);
    }
    if (auto written = write_recording(recording, requests, scenario.ticker); !written) {
        return std::unexpected{written.error()};
    }
    // The replay runs in the work directory, where it writes its own log too. Absolute paths, since
    // its working directory isn't this one.
    if (auto replayed = run_in(paths.work, paths.work / "replay.log", std::filesystem::absolute(paths.exchange_replay),
                               {std::filesystem::absolute(recording).string(), std::filesystem::absolute(market_data).string(),
                                "--steps", std::filesystem::absolute(steps_file).string()});
        !replayed) {
        return std::unexpected{replayed.error()};
    }
    const auto market = read_market_data_file(market_data);
    if (!market) {
        return std::unexpected{market.error()};
    }
    const auto steps = read_steps_file(steps_file);
    if (!steps) {
        return std::unexpected{steps.error()};
    }
    const auto checked = check_scenario(scenario, market->updates, *steps);
    if (!checked) {
        return std::unexpected{checked.error()};
    }
    if (const std::string wrong = false_claims(*checked); !wrong.empty()) {
        return std::unexpected{std::format("a claim is false, so the answer isn't written:\n{}", wrong)};
    }
    const auto fingerprint = fingerprint_file(market_data);
    if (!fingerprint) {
        return std::unexpected{fingerprint.error()};
    }
    const std::string text = answer_json(scenario, scenario_json, *checked, market_data, *fingerprint).dump(1) + "\n";
    return write_atomically(paths.answer_file, text);
}

}  // namespace fillscope
