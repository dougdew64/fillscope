#include "feed/time_shift.h"

#include <algorithm>
#include <format>

namespace fillscope {

std::expected<void, std::string> apply_to(Moment& moment, const MarketUpdate& update) {
    if (update.ticker >= BookBuilder::kTickers) {
        return std::unexpected{std::format("update {} is for ticker {}, and the exchange has {}", update.seq, update.ticker,
                                           BookBuilder::kTickers)};
    }
    if (update.type == UpdateType::trade) {
        moment.last_trades[update.ticker] = update;
    } else if (auto applied = moment.books[update.ticker].apply(update); !applied) {
        return applied;
    }
    moment.seq = update.seq;
    return {};
}

Moment TimeShiftBuffer::copy_of(const BookBuilder& live) {
    Moment moment;
    moment.seq = live.last_seq();
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        moment.books[ticker] = live.book(ticker);
        moment.last_trades[ticker] = live.last_trade(ticker);
    }
    return moment;
}

void TimeShiftBuffer::received(const MarketUpdate& update, Clock::time_point arrived) {
    pending_.try_emplace(update.seq, Kept{.update = update, .arrived = arrived});
}

void TimeShiftBuffer::observe(const BookBuilder& live) {
    if (live.status() != BookBuilder::Status::live) {
        return;  // waiting, or recovering: the run, if any, ended where the gap began
    }
    const std::uint64_t last_seq = live.last_seq();
    if (live.recoveries().size() > recoveries_seen_) {
        // Rebuilt from a snapshot, and maybe the updates queued after it applied too: a new run, from
        // the books as they are now. The updates before them were never applied one at a time.
        recoveries_seen_ = live.recoveries().size();
        runs_.push_back(Run{.first = last_seq, .last = last_seq});
        data_.push_back(RunData{.updates = {}, .checkpoints = {copy_of(live)}});
        pending_.erase(pending_.begin(), pending_.upper_bound(last_seq));
        return;
    }
    if (runs_.empty()) {
        // Live from update 1, with no recovery: the first run starts before it, from empty books.
        runs_.push_back(Run{.first = 0, .last = 0});
        data_.push_back(RunData{.updates = {}, .checkpoints = {Moment{}}});
    }
    Run& run = runs_.back();
    RunData& data = data_.back();
    // The updates the books applied since: run.last + 1 up to last_seq, each received before.
    while (run.last < last_seq) {
        const auto next = pending_.find(run.last + 1);
        if (next == pending_.end()) {
            return;  // can't happen: the books applied an update never received
        }
        data.updates.push_back(next->second);
        ++run.last;
        ++kept_;
        pending_.erase(pending_.begin(), std::next(next));
    }
    if (run.last - data.checkpoints.back().seq >= kCheckpointEvery) {
        data.checkpoints.push_back(copy_of(live));
    }
}

TimeShiftBuffer TimeShiftBuffer::from_replay(std::span<const MarketUpdate> updates) {
    TimeShiftBuffer buffer;
    buffer.runs_.push_back(Run{.first = 0, .last = 0});
    buffer.data_.push_back(RunData{.updates = {}, .checkpoints = {Moment{}}});
    Run& run = buffer.runs_.back();
    RunData& data = buffer.data_.back();
    Moment books;
    for (const MarketUpdate& update : updates) {
        if (update.seq != run.last + 1 || !apply_to(books, update)) {
            break;  // the books can't go further, as the viewer's replay stops there
        }
        data.updates.push_back(Kept{.update = update, .arrived = std::nullopt});
        ++run.last;
        ++buffer.kept_;
        if (run.last - data.checkpoints.back().seq >= kCheckpointEvery) {
            data.checkpoints.push_back(books);
        }
    }
    return buffer;
}

std::optional<std::uint64_t> TimeShiftBuffer::newest() const {
    if (runs_.empty()) {
        return std::nullopt;
    }
    return runs_.back().last;
}

std::optional<std::size_t> TimeShiftBuffer::run_of(std::uint64_t seq) const {
    // The last run starting at or before seq.
    const auto after = std::ranges::upper_bound(runs_, seq, {}, &Run::first);
    if (after == runs_.begin()) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(std::distance(runs_.begin(), after) - 1);
    if (seq > runs_[index].last) {
        return std::nullopt;
    }
    return index;
}

std::optional<std::size_t> TimeShiftBuffer::updates_to_apply(std::uint64_t seq) const {
    const auto run = run_of(seq);
    if (!run) {
        return std::nullopt;
    }
    const auto& checkpoints = data_[*run].checkpoints;
    const auto from = std::ranges::upper_bound(checkpoints, seq, {}, &Moment::seq) - 1;
    return static_cast<std::size_t>(seq - from->seq);
}

std::expected<Moment, std::string> TimeShiftBuffer::moment(std::uint64_t seq) const {
    const auto run = run_of(seq);
    if (!run) {
        if (runs_.empty()) {
            return std::unexpected{std::string{"nothing has been kept yet"}};
        }
        if (seq < runs_.front().first) {
            return std::unexpected{std::format("the books are kept from update {} on", runs_.front().first)};
        }
        if (seq > runs_.back().last) {
            return std::unexpected{std::format("update {} hasn't arrived; the newest kept is {}", seq, runs_.back().last)};
        }
        const auto after = std::ranges::upper_bound(runs_, seq, {}, &Run::first);
        return std::unexpected{std::format("update {} is in a gap: after update {} the books were rebuilt from a snapshot as of "
                                           "update {}, so the updates between were never applied one by one",
                                           seq, std::prev(after)->last, after->first)};
    }
    const RunData& data = data_[*run];
    const std::uint64_t first = runs_[*run].first;
    const auto from = std::ranges::upper_bound(data.checkpoints, seq, {}, &Moment::seq) - 1;
    Moment moment = *from;
    for (std::uint64_t next = from->seq + 1; next <= seq; ++next) {
        if (auto applied = apply_to(moment, data.updates[next - first - 1].update); !applied) {
            // The live books applied it, so this can't happen; said rather than shown wrong.
            return std::unexpected{std::format("update {} can't be applied again: {}", next, applied.error())};
        }
    }
    moment.seq = seq;
    return moment;
}

std::optional<TimeShiftBuffer::Clock::time_point> TimeShiftBuffer::arrived(std::uint64_t seq) const {
    const auto run = run_of(seq);
    if (!run || seq == runs_[*run].first) {
        return std::nullopt;  // a run's start was rebuilt from a snapshot, or is before update 1
    }
    return data_[*run].updates[seq - runs_[*run].first - 1].arrived;
}

const MarketUpdate* TimeShiftBuffer::update(std::uint64_t seq) const {
    const auto run = run_of(seq);
    if (!run || seq == runs_[*run].first) {
        return nullptr;
    }
    return &data_[*run].updates[seq - runs_[*run].first - 1].update;
}

std::optional<std::uint64_t> TimeShiftBuffer::next(std::uint64_t seq) const {
    const auto run = run_of(seq);
    if (!run) {
        return std::nullopt;
    }
    if (seq < runs_[*run].last) {
        return seq + 1;
    }
    if (*run + 1 < runs_.size()) {
        return runs_[*run + 1].first;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> TimeShiftBuffer::previous(std::uint64_t seq) const {
    const auto run = run_of(seq);
    if (!run) {
        return std::nullopt;
    }
    if (seq > runs_[*run].first) {
        return seq - 1;
    }
    if (*run > 0) {
        return runs_[*run - 1].last;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> TimeShiftBuffer::nearest_shown(std::uint64_t seq) const {
    if (runs_.empty()) {
        return std::nullopt;
    }
    for (const Run& run : runs_) {
        if (seq <= run.last) {
            return std::max(seq, run.first);
        }
    }
    return runs_.back().last;
}

}  // namespace fillscope
