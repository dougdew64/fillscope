// An answer the viewer shows (answer/answer.h makes them, docs/ARCHITECTURE.md §10): read from its
// file, its claims checked here again against the replay's own files rather than taken from the
// file's word, and the frames it is stepped through.
#pragma once

#include "answer/answer.h"
#include "answer/scenario.h"
#include "feed/book.h"
#include "feed/market_data_file.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fillscope {

// One place in an answer, as the user steps through it: the intro before any step, a step's
// question before the step is played, or a step played. The books are after update `books_after`.
struct AnswerFrame {
    std::size_t step = 0;  // 0 for the intro; otherwise the scenario's step, from 1
    bool asking = false;   // the step's question, the books still as they were before it
    std::uint64_t books_after = 0;

    friend bool operator==(const AnswerFrame&, const AnswerFrame&) = default;
};

// The intro, then for each step its question (if it asks one) and the step played.
[[nodiscard]] std::vector<AnswerFrame> answer_frames(const Scenario& scenario, const std::vector<CheckedStep>& checked);

struct LoadedAnswer {
    Scenario scenario;
    std::vector<CheckedStep> checked;  // this viewer's own check of every claim
    std::filesystem::path market_data_file;
    MarketDataFile market_data;
    std::vector<AnswerFrame> frames;
    // Whose each order is, by the exchange's id for it: the client's role and its own id ("the
    // maker's 11"). The market data names no client; an answer's replay knows, from the responses
    // that accepted each order, so the book can say.
    std::map<std::uint64_t, std::string> owners;
};

// The answer in `file`, or why it can't be shown: not an answer, a scenario that can't be read, a
// replay file missing, or one changed since the answer was written (its fingerprint differs), in
// which case the answer is stale and the tool has to be run again.
[[nodiscard]] std::expected<LoadedAnswer, std::string> load_answer(const std::filesystem::path& file);

// One market update of an answer's step, in words, with whose order it touches where the answer
// knows ("the exchange's order 2 (the maker's 11)"). In a step that sends a new order, a cancel
// update can only be a resting order filled whole, and a modify one partly filled (the back end's
// MEOrderBook::match); in a step that cancels, it is the cancel. A trade's market data names no
// order, so its incoming side's owner isn't said.
[[nodiscard]] std::string describe_update(const MarketUpdate& update, const LoadedAnswer& answer, bool cancel_step);

// The book's best bid and best ask, whose they are, and the spread between them, in words.
[[nodiscard]] std::string describe_top(const Book& book, const LoadedAnswer& answer);

// What identifies one version of a file, to see it has been replaced: the answer tool renames a new
// file over the old one, which changes all three.
struct FileStamp {
    std::uintmax_t inode = 0;
    std::uintmax_t size = 0;
    std::int64_t modified_ns = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

// The file's stamp, or nothing if it doesn't exist.
[[nodiscard]] std::optional<FileStamp> stamp_of(const std::filesystem::path& file);

// The answer the viewer watches for, and what it shows of it.
struct AnswerShown {
    std::filesystem::path file;              // the answer file watched, as an absolute path
    std::optional<FileStamp> stamp;          // the version last read, if any
    std::optional<LoadedAnswer> answer;      // the answer shown, if the last read gave one
    std::optional<std::string> problem;      // why the last read gave none, if it didn't
    std::size_t frame = 0;                   // where in answer->frames the user is
};

}  // namespace fillscope
