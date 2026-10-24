#include "feed/book_builder.h"

#include <format>
#include <utility>

namespace fillscope {

void BookBuilder::on_incremental(const MarketUpdate& update) {
    ++updates_received_;
    if (status_ == Status::recovering) {
        queued_incremental_[update.seq] = update;
        try_to_sync();
        return;
    }
    if (update.seq != next_seq_) {
        start_recovery(status_ == Status::waiting
                           ? std::format("the first update to arrive was {}: the feed was already running", update.seq)
                           : std::format("update {} arrived when {} was next", update.seq, next_seq_));
        queued_incremental_[update.seq] = update;
        try_to_sync();
        return;
    }
    std::string problem;
    if (!apply_to_books(update, false, update.seq, problem)) {
        // The books didn't change. Queued, so it is applied again after the snapshot if the snapshot
        // doesn't include it.
        start_recovery(std::move(problem));
        queued_incremental_[update.seq] = update;
        return;
    }
    status_ = Status::live;
    ++next_seq_;
}

void BookBuilder::on_snapshot(const MarketUpdate& update) {
    if (status_ != Status::recovering) {
        return;
    }
    if (queued_snapshot_.contains(update.seq)) {
        queued_snapshot_.clear();  // a number seen again: a new snapshot has begun
    }
    queued_snapshot_[update.seq] = update;
    try_to_sync();
}

void BookBuilder::start_recovery(std::string reason) {
    status_ = Status::recovering;
    recovering_because_ = std::move(reason);
    recovery_after_ = last_seq();
    queued_snapshot_.clear();
}

void BookBuilder::try_to_sync() {
    if (queued_snapshot_.empty()) {
        return;
    }
    // A snapshot is usable once it is whole: a start numbered 0, every update after it in sequence,
    // and an end. A snapshot whose start was missed, or with a gap, can't be: wait for the next.
    if (queued_snapshot_.begin()->first != 0 || queued_snapshot_.begin()->second.type != UpdateType::snapshot_start) {
        queued_snapshot_.clear();
        return;
    }
    std::uint64_t expected = 0;
    for (const auto& [seq, update] : queued_snapshot_) {
        if (seq != expected++) {
            queued_snapshot_.clear();
            return;
        }
    }
    if (queued_snapshot_.rbegin()->second.type != UpdateType::snapshot_end) {
        return;  // not whole yet
    }
    const std::uint64_t snapshot_seq = queued_snapshot_.begin()->second.order_id;
    if (queued_snapshot_.rbegin()->second.order_id != snapshot_seq) {
        // The start of one snapshot and the end of another, joined where updates were lost.
        queued_snapshot_.clear();
        return;
    }

    // The queued incremental updates after the snapshot must follow it without a gap; if one is
    // missing, so is what the books need from it: wait for the next snapshot.
    std::uint64_t next = snapshot_seq + 1;
    for (const auto& [seq, update] : queued_incremental_) {
        if (seq < next) {
            continue;  // the snapshot includes it
        }
        if (seq != next) {
            queued_snapshot_.clear();
            return;
        }
        ++next;
    }

    // Rebuild every book from the snapshot (its updates include a clear for each ticker). If its
    // orders don't make a consistent book, everything is put back as it was: the books, the trades,
    // and the events the snapshot added to the histories.
    auto saved_books = books_;
    auto saved_trades = last_trades_;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> added;  // whose history gained an event
    for (std::size_t ticker = 0; ticker < kTickers; ++ticker) {
        books_[ticker].clear();
        last_trades_[ticker].reset();
    }
    std::string problem;
    for (const auto& [seq, update] : queued_snapshot_) {
        if (update.type == UpdateType::snapshot_start || update.type == UpdateType::snapshot_end) {
            continue;
        }
        if (!apply_to_books(update, true, snapshot_seq, problem)) {
            books_ = std::move(saved_books);
            last_trades_ = std::move(saved_trades);
            for (const auto& [ticker, order_id] : added) {
                auto& events = histories_[ticker][order_id];
                events.pop_back();
                if (events.empty()) {
                    histories_[ticker].erase(order_id);
                }
            }
            recovering_because_ = std::format("the snapshot of update {} doesn't make a consistent book: {}", snapshot_seq, problem);
            queued_snapshot_.clear();
            return;
        }
        if (update.type != UpdateType::clear && update.type != UpdateType::trade) {  // those add no event
            added.emplace_back(update.ticker, update.order_id);
        }
    }
    next_seq_ = snapshot_seq + 1;
    // The books are rebuilt: whatever happens to the queued updates, this is a recovery.
    recoveries_.push_back(Recovery{.after_seq = recovery_after_, .snapshot_seq = snapshot_seq,
                                   .reason = recovering_because_.value_or("")});
    for (auto queued = queued_incremental_.begin(); queued != queued_incremental_.end();) {
        if (queued->first < next_seq_) {
            queued = queued_incremental_.erase(queued);
            continue;
        }
        if (!apply_to_books(queued->second, false, queued->first, problem)) {
            // The books are as of next_seq_ - 1; this update and those after it stay queued, and a
            // recovery from there begins.
            recovering_because_ = std::move(problem);
            recovery_after_ = last_seq();
            queued_snapshot_.clear();
            return;
        }
        ++next_seq_;
        queued = queued_incremental_.erase(queued);
    }
    status_ = Status::live;
    recovering_because_.reset();
    queued_snapshot_.clear();
}

bool BookBuilder::apply_to_books(const MarketUpdate& update, bool from_snapshot, std::uint64_t seq, std::string& problem) {
    if (update.ticker >= kTickers) {
        problem = std::format("update {} is for ticker {}, and the exchange has {}", update.seq, update.ticker, kTickers);
        return false;
    }
    switch (update.type) {
    case UpdateType::trade:
        last_trades_[update.ticker] = update;
        return true;
    case UpdateType::clear:
        books_[update.ticker].clear();
        last_trades_[update.ticker].reset();
        return true;
    default:
        break;
    }
    if (const auto applied = books_[update.ticker].apply(update); !applied) {
        problem = applied.error();
        return false;
    }
    histories_[update.ticker][update.order_id].push_back(OrderEvent{.seq = seq,
                                                                    .type = update.type,
                                                                    .side = update.side,
                                                                    .price = update.price,
                                                                    .qty = update.qty,
                                                                    .from_snapshot = from_snapshot});
    return true;
}

std::span<const OrderEvent> BookBuilder::history(std::uint32_t ticker, std::uint64_t order_id) const {
    const auto& orders = histories_.at(ticker);
    const auto found = orders.find(order_id);
    if (found == orders.end()) {
        return {};
    }
    return found->second;
}

}  // namespace fillscope
