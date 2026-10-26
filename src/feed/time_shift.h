// What the viewer keeps of the market data it receives, so it can show the books exactly as they
// were after any update it kept, as a television recorder plays back what it recorded
// (docs/ARCHITECTURE.md, §3, *Keeping what arrives*).
//
// The live books (BookBuilder) are given each update as it arrives; this keeps, beside them, the
// updates they applied, each with the time it arrived, in runs. A run starts before update 1, or
// where the books were rebuilt from a snapshot after a gap, and ends where the next gap began: the
// books between two runs were never built one update at a time, so they can't be shown. Every
// kCheckpointEvery updates, a copy of the live books (their resting orders, small beside the
// updates) is kept too, so the books after any kept update are a checkpoint's copy and at most
// kCheckpointEvery - 1 updates applied to it.
//
// A replay's market data is held the same way: one run, from before update 1, with no times.
#pragma once

#include "feed/book.h"
#include "feed/book_builder.h"
#include "feed/market_update.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fillscope {

// The books as they were after one update: every ticker's book, and its last trade (BookBuilder's).
struct Moment {
    std::uint64_t seq = 0;  // after this update; 0 is before update 1
    std::array<Book, BookBuilder::kTickers> books;
    std::array<std::optional<MarketUpdate>, BookBuilder::kTickers> last_trades;
};

class TimeShiftBuffer {
public:
    using Clock = std::chrono::steady_clock;

    // Measured (docs/ARCHITECTURE.md, §3, *Keeping what arrives*): going to any update of an hour's
    // session takes well under a frame.
    static constexpr std::size_t kCheckpointEvery = 1024;

    // A run: the books can be shown as they were after each update from `first` to `last`.
    struct Run {
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        friend bool operator==(const Run&, const Run&) = default;
    };

    // An incremental update as it arrives, before the live books are given it. Only the first
    // arrival of an update number counts.
    void received(const MarketUpdate& update, Clock::time_point arrived);

    // After the live books were given an update, incremental or snapshot: keeps what they applied.
    void observe(const BookBuilder& live);

    // A replay's market data, numbered from 1: one run, from 0 to the last update the books could
    // apply (a replay's updates stop there, as the viewer's replay does), with no arrival times.
    [[nodiscard]] static TimeShiftBuffer from_replay(std::span<const MarketUpdate> updates);

    // Building one from what was written of another (feed/kept_file.h): a run starting from `base`
    // (after a gap, if one came before: base.seq must be past the last run's end), then each update
    // after it, in order, applied to the books for the checkpoints. Or why the update can't follow.
    [[nodiscard]] std::expected<void, std::string> start_run(Moment base);
    [[nodiscard]] std::expected<void, std::string> append(const MarketUpdate& update, std::optional<Clock::time_point> arrived);

    [[nodiscard]] const std::vector<Run>& runs() const noexcept { return runs_; }
    // The newest update kept, if any.
    [[nodiscard]] std::optional<std::uint64_t> newest() const;
    // The books as they were after update `seq`, or why they can't be shown: before the first
    // update kept, after the newest, or in a gap, which says what happened there.
    [[nodiscard]] std::expected<Moment, std::string> moment(std::uint64_t seq) const;
    // When update `seq` arrived, if it was kept with a time (a replay's have none).
    [[nodiscard]] std::optional<Clock::time_point> arrived(std::uint64_t seq) const;
    // How many updates moment(seq) applies to a checkpoint's copy, if `seq` can be shown: at most
    // kCheckpointEvery - 1.
    [[nodiscard]] std::optional<std::size_t> updates_to_apply(std::uint64_t seq) const;
    // Kept update `seq`, if it was kept as an update (a run's start was rebuilt from a snapshot).
    [[nodiscard]] const MarketUpdate* update(std::uint64_t seq) const;
    // The next update that can be shown after `seq`: the next in its run, or, at a run's end, the
    // start of the next run, across the gap; nothing at the newest.
    [[nodiscard]] std::optional<std::uint64_t> next(std::uint64_t seq) const;
    // The update before `seq` that can be shown: the one before in its run, or, at a run's start, the
    // last of the run before, across the gap; nothing at the first.
    [[nodiscard]] std::optional<std::uint64_t> previous(std::uint64_t seq) const;
    // The nearest update at or after `seq` that can be shown, or the newest if there is none after.
    [[nodiscard]] std::optional<std::uint64_t> nearest_shown(std::uint64_t seq) const;

    // The updates kept, over all runs.
    [[nodiscard]] std::size_t kept() const noexcept { return kept_; }

private:
    struct Kept {
        MarketUpdate update;
        std::optional<Clock::time_point> arrived;
    };
    struct RunData {
        std::vector<Kept> updates;        // first + 1, first + 2, ...
        std::vector<Moment> checkpoints;  // by seq; the first is the run's start
        std::optional<Moment> tip;        // built by append: the books after the run's last update
    };

    [[nodiscard]] static Moment copy_of(const BookBuilder& live);
    // The run `seq` is in, or nothing.
    [[nodiscard]] std::optional<std::size_t> run_of(std::uint64_t seq) const;

    std::vector<Run> runs_;
    std::vector<RunData> data_;
    std::map<std::uint64_t, Kept> pending_;  // received, not yet applied by the live books
    std::size_t recoveries_seen_ = 0;
    std::size_t kept_ = 0;
};

// Applies an incremental update to a moment as BookBuilder applies it to its books: a trade becomes
// the ticker's last trade; an add, modify or cancel changes its book. Or why it can't.
[[nodiscard]] std::expected<void, std::string> apply_to(Moment& moment, const MarketUpdate& update);

}  // namespace fillscope
