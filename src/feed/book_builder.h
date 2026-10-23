// Every ticker's book, kept in step with the exchange's market data.
//
// The exchange publishes two streams (backend/, market_data_publisher.cpp): incremental updates,
// numbered 1, 2, 3, ...; and, every so often, a snapshot of every book, whose start names the last
// incremental update it includes. A reader applies incremental updates in order. If one is missing
// (a gap: multicast doesn't retransmit), or it joins a feed already running, it can't know what it
// missed, so it recovers: it queues incremental updates until a complete snapshot arrives, rebuilds
// every book from it, and applies the queued updates that follow it, which must continue from the
// snapshot without a gap. This is the protocol the book's own trading client follows
// (MarketDataConsumer, trading/market_data/market_data_consumer.cpp there).
#pragma once

#include "feed/book.h"
#include "feed/market_update.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fillscope {

// An update the books applied to one order, for following it.
struct OrderEvent {
    std::uint64_t seq = 0;  // the incremental update's number; for a snapshot, the last it includes
    UpdateType type = UpdateType::add;
    Side side = Side::none;
    std::int64_t price = 0;
    std::uint32_t qty = 0;
    bool from_snapshot = false;  // the order was found resting in a snapshot, not seen arriving

    friend bool operator==(const OrderEvent&, const OrderEvent&) = default;
};

// A recovery that completed: the books were rebuilt from a snapshot, so the incremental updates
// after `after_seq` up to `snapshot_seq` were never applied one by one, and an order's history has
// no events from them.
struct Recovery {
    std::uint64_t after_seq = 0;     // the last update applied before the gap (0: none)
    std::uint64_t snapshot_seq = 0;  // the last update the snapshot included
    std::string reason;              // why recovery started

    friend bool operator==(const Recovery&, const Recovery&) = default;
};

class BookBuilder {
public:
    // The exchange's tickers: 0 to 7 (ME_MAX_TICKERS, common/types.h there).
    static constexpr std::size_t kTickers = 8;

    enum class Status {
        waiting,     // nothing has arrived yet
        live,        // the books are current, as of last_seq()
        recovering,  // updates were missed; waiting for a snapshot to rebuild from
    };

    void on_incremental(const MarketUpdate& update);
    // Snapshot updates are used only while recovering, as the trading client uses them.
    void on_snapshot(const MarketUpdate& update);

    [[nodiscard]] Status status() const noexcept { return status_; }
    // The last incremental update the books include (0: none). While recovering, the books are as
    // of this update, and older than the feed.
    [[nodiscard]] std::uint64_t last_seq() const noexcept { return next_seq_ - 1; }
    // Why the current recovery started; empty when not recovering.
    [[nodiscard]] const std::optional<std::string>& recovering_because() const noexcept { return recovering_because_; }
    [[nodiscard]] std::span<const Recovery> recoveries() const noexcept { return recoveries_; }
    [[nodiscard]] std::uint64_t updates_received() const noexcept { return updates_received_; }

    [[nodiscard]] const Book& book(std::uint32_t ticker) const { return books_.at(ticker); }
    // The ticker's last trade, if one has been seen since the last recovery.
    [[nodiscard]] const std::optional<MarketUpdate>& last_trade(std::uint32_t ticker) const { return last_trades_.at(ticker); }

    // What the books applied to this order, in order; empty if it was never seen. Kept for every order,
    // about 40 bytes an event: a long run's orders take memory in proportion to the run.
    [[nodiscard]] std::span<const OrderEvent> history(std::uint32_t ticker, std::uint64_t order_id) const;

private:
    void start_recovery(std::string reason);
    void try_to_sync();
    [[nodiscard]] bool apply_to_books(const MarketUpdate& update, bool from_snapshot, std::uint64_t seq,
                                      std::string& problem);

    Status status_ = Status::waiting;
    std::uint64_t next_seq_ = 1;
    std::uint64_t updates_received_ = 0;
    std::array<Book, kTickers> books_;
    std::array<std::optional<MarketUpdate>, kTickers> last_trades_;
    std::array<std::unordered_map<std::uint64_t, std::vector<OrderEvent>>, kTickers> histories_;

    std::optional<std::string> recovering_because_;
    std::uint64_t recovery_after_ = 0;                          // last_seq() when recovery started
    std::map<std::uint64_t, MarketUpdate> queued_incremental_;  // while recovering, by number
    std::map<std::uint64_t, MarketUpdate> queued_snapshot_;     // while recovering, by number in the snapshot
    std::vector<Recovery> recoveries_;
};

}  // namespace fillscope
