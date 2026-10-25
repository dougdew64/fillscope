#include "viewer/answer_state.h"

#include "answer/wire.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <format>
#include <fstream>

namespace fillscope {

std::vector<AnswerFrame> answer_frames(const Scenario& scenario, const std::vector<CheckedStep>& checked) {
    std::vector<AnswerFrame> frames{{.step = 0, .asking = false, .books_after = 0}};
    for (std::size_t i = 0; i < checked.size() && i < scenario.steps.size(); ++i) {
        // first_update is the step's first update, or the next one's if it made none, so the books
        // before the step are after first_update - 1.
        const std::uint64_t before = checked[i].first_update - 1;
        if (!scenario.steps[i].ask.empty()) {
            frames.push_back({.step = i + 1, .asking = true, .books_after = before});
        }
        frames.push_back({.step = i + 1, .asking = false, .books_after = before + checked[i].update_count});
    }
    return frames;
}

std::expected<LoadedAnswer, std::string> load_answer(const std::filesystem::path& file) {
    std::ifstream in{file};
    if (!in) {
        return std::unexpected{std::format("there is no answer at {}", file.string())};
    }
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object() || json.value("version", 0) != 1) {
        return std::unexpected{std::format("{} isn't an answer the viewer can read", file.string())};
    }
    if (!json.contains("scenario") || !json.contains("replay") || !json["replay"].is_object()) {
        return std::unexpected{std::format("{} has no scenario or replay", file.string())};
    }
    auto scenario = read_scenario(json["scenario"]);
    if (!scenario) {
        return std::unexpected{std::format("its scenario can't be read: {}", scenario.error())};
    }
    const nlohmann::json& replay = json["replay"];
    for (const char* field : {"marketDataFile", "marketDataFingerprint", "stepsFile", "stepsFingerprint"}) {
        if (!replay.contains(field) || !replay[field].is_string()) {
            return std::unexpected{std::format("its replay has no {}", field)};
        }
    }
    const std::filesystem::path market_file = replay["marketDataFile"].get<std::string>();
    const std::filesystem::path steps_file = replay["stepsFile"].get<std::string>();
    // The files must be the ones the answer was checked against: a replay since run over them would
    // make every step describe something else.
    for (const auto& [path, recorded] : {std::pair{&market_file, replay["marketDataFingerprint"].get<std::string>()},
                                         std::pair{&steps_file, replay["stepsFingerprint"].get<std::string>()}}) {
        const auto now = fingerprint_file(*path);
        if (!now) {
            return std::unexpected{std::format("the answer's replay is gone: {}", now.error())};
        }
        if (*now != recorded) {
            return std::unexpected{std::format("the answer is stale: {} has changed since it was written", path->string())};
        }
    }
    auto market_data = read_market_data_file(market_file);
    if (!market_data) {
        return std::unexpected{market_data.error()};
    }
    const auto steps = read_steps_file(steps_file);
    if (!steps) {
        return std::unexpected{steps.error()};
    }
    auto checked = check_scenario(*scenario, market_data->updates, *steps);
    if (!checked) {
        return std::unexpected{std::format("its replay doesn't fit its scenario: {}", checked.error())};
    }
    LoadedAnswer loaded{.scenario = std::move(*scenario),
                        .checked = std::move(*checked),
                        .market_data_file = market_file,
                        .market_data = std::move(*market_data),
                        .frames = {},
                        .owners = {}};
    loaded.frames = answer_frames(loaded.scenario, loaded.checked);
    for (const CheckedStep& step : loaded.checked) {
        for (const ClientResponse& response : step.responses) {
            if (response.type == ResponseType::accepted) {
                loaded.owners[response.market_order_id] =
                    std::format("{}'s {}", role_of(loaded.scenario, response.client), response.client_order_id);
            }
        }
    }
    return loaded;
}

namespace {

// "the exchange's order 2 (the maker's 11)", or without the owner if the answer doesn't know it.
std::string order_named(std::uint64_t id, const LoadedAnswer& answer) {
    const auto owner = answer.owners.find(id);
    return owner != answer.owners.end() ? std::format("the exchange's order {} ({})", id, owner->second)
                                        : std::format("the exchange's order {}", id);
}

std::string_view side_word(Side side) { return side == Side::buy ? "buy" : "sell"; }

}  // namespace

std::string describe_update(const MarketUpdate& update, const LoadedAnswer& answer, bool cancel_step) {
    const std::string at = std::format("update {}", update.seq);
    switch (update.type) {
    case UpdateType::add:
        return std::format("{}: {} rests in the book: {} {} at {}", at, order_named(update.order_id, answer),
                           side_word(update.side), update.qty, update.price);
    case UpdateType::modify:
        return std::format("{}: {} now has {}: it was partly filled", at, order_named(update.order_id, answer), update.qty);
    case UpdateType::cancel:
        return std::format("{}: {} leaves the book: {}", at, order_named(update.order_id, answer),
                           cancel_step ? "it was canceled" : "it was filled, all of it");
    case UpdateType::trade:
        return std::format("{}: a trade, {} at {}, caused by an incoming {}; the market data doesn't say whose", at,
                           update.qty, update.price, side_word(update.side));
    case UpdateType::clear:
    case UpdateType::snapshot_start:
    case UpdateType::snapshot_end:
        break;
    }
    return std::format("{}: {}", at, to_string(update.type));
}

std::string describe_top(const Book& book, const LoadedAnswer& answer) {
    const auto bids = book.levels(Side::buy);
    const auto asks = book.levels(Side::sell);
    const auto best = [&answer](const std::vector<Level>& levels, std::string_view name) {
        if (levels.empty()) {
            return std::format("no {}", name);
        }
        const Level& level = levels.front();
        // The first order at the best price is the one that fills next.
        const auto owner = answer.owners.find(level.orders.front().order_id);
        return std::format("best {} {} for {}{}", name, level.price, level.qty,
                           owner != answer.owners.end() ? std::format(" (first in line: {})", owner->second) : std::string{});
    };
    std::string text = std::format("{}; {}", best(bids, "bid"), best(asks, "ask"));
    if (!bids.empty() && !asks.empty()) {
        text += std::format("; a spread of {}", asks.front().price - bids.front().price);
    }
    return text;
}

std::optional<FileStamp> stamp_of(const std::filesystem::path& file) {
    struct stat status{};
    if (::stat(file.c_str(), &status) != 0) {
        return std::nullopt;
    }
    return FileStamp{.inode = static_cast<std::uintmax_t>(status.st_ino),
                     .size = static_cast<std::uintmax_t>(status.st_size),
                     .modified_ns = static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000 + status.st_mtim.tv_nsec};
}

}  // namespace fillscope
