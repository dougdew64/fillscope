// A simulated exchange's market data, and a viewer's live books and what it keeps given it, for the
// tests of what the viewer keeps (time_shift_test.cpp, kept_file_test.cpp).
#pragma once

#include "feed/time_shift.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace fillscope::test {

using Clock = TimeShiftBuffer::Clock;
using Type = UpdateType;

// A simulated exchange's market data: adds, partial fills (modify), cancels and trades on every
// ticker, numbered from 1, each consistent with the books before it; and, at any point, a snapshot of
// those books, as the exchange sends it.
class Exchange {
public:
    explicit Exchange(unsigned seed) : random_{seed} {}

    MarketUpdate next() {
        MarketUpdate update{.seq = ++seq_};
        update.ticker = pick(BookBuilder::kTickers);
        std::map<std::uint64_t, MarketUpdate>& resting = resting_[update.ticker];
        const unsigned roll = pick(10);
        // Adds while a book is thin; once it holds 25 orders, more cancels and fills than adds, so it
        // stays the size of a real book.
        if (resting.size() < 5 || (resting.size() < 25 && roll < 4) || roll < 2) {
            update.type = Type::add;
            update.order_id = ++order_id_;
            update.side = pick(2) == 0 ? Side::buy : Side::sell;
            update.price = 100 + static_cast<std::int64_t>(pick(10)) + (update.side == Side::sell ? 10 : 0);
            update.qty = 1 + pick(100);
            update.priority = ++priority_;
            resting[update.order_id] = update;
        } else if (roll < 6) {
            update = trade_on(update, resting);
        } else {
            auto order = resting.begin();
            std::advance(order, pick(static_cast<unsigned>(resting.size())));
            update.order_id = order->first;
            update.side = order->second.side;
            update.price = order->second.price;
            update.priority = order->second.priority;
            if (roll < 8 && order->second.qty > 1) {
                update.type = Type::modify;  // partly filled: less left, its place kept
                update.qty = order->second.qty = 1 + pick(order->second.qty - 1);
            } else {
                update.type = Type::cancel;
                resting.erase(order);
            }
        }
        return update;
    }

    // A whole snapshot of the books after the last update made, numbered from 0.
    std::vector<MarketUpdate> snapshot() const {
        std::vector<MarketUpdate> updates{{.type = Type::snapshot_start, .order_id = seq_}};
        for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
            updates.push_back({.type = Type::clear, .ticker = ticker});
            for (const auto& [id, order] : resting_[ticker]) {
                updates.push_back(order);
            }
        }
        updates.push_back({.type = Type::snapshot_end, .order_id = seq_});
        for (std::uint64_t i = 0; i < updates.size(); ++i) {
            updates[i].seq = i;
        }
        return updates;
    }

private:
    unsigned pick(unsigned n) { return std::uniform_int_distribution<unsigned>{0, n - 1}(random_); }
    MarketUpdate trade_on(MarketUpdate update, const std::map<std::uint64_t, MarketUpdate>& resting) {
        const MarketUpdate& order = resting.begin()->second;
        update.type = Type::trade;
        update.side = order.side == Side::buy ? Side::sell : Side::buy;
        update.price = order.price;
        update.qty = 1;
        return update;
    }

    std::mt19937 random_;
    std::uint64_t seq_ = 0;
    std::uint64_t order_id_ = 0;
    std::uint64_t priority_ = 0;
    std::array<std::map<std::uint64_t, MarketUpdate>, BookBuilder::kTickers> resting_;
};

inline Moment copy_of(const BookBuilder& books) {
    Moment moment;
    moment.seq = books.last_seq();
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        moment.books[ticker] = books.book(ticker);
        moment.last_trades[ticker] = books.last_trade(ticker);
    }
    return moment;
}

inline void expect_same(const Moment& kept, const Moment& live) {
    EXPECT_EQ(kept.seq, live.seq);
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        EXPECT_EQ(kept.books[ticker].orders(), live.books[ticker].orders()) << "update " << live.seq << ", ticker " << ticker;
        EXPECT_EQ(kept.last_trades[ticker], live.last_trades[ticker]) << "update " << live.seq << ", ticker " << ticker;
    }
}

// The live books and the buffer, given a feed as the viewer gives it: each incremental update
// received and then applied, each snapshot update applied, the buffer looking after each. Every
// update the live books were live at is noted; the buffer's moment is compared with the live books
// there and then, for those that `check` picks, and for the last before each gap (where a run ends).
struct Viewer {
    BookBuilder books;
    TimeShiftBuffer kept;
    std::set<std::uint64_t> seen;  // the updates the live books were live at
    std::map<std::uint64_t, Clock::time_point> first_arrival;
    Clock::time_point now = Clock::time_point{} + std::chrono::hours{1};
    std::function<bool(std::uint64_t)> check = [](std::uint64_t) { return true; };
    bool was_live = false;
    std::size_t compared = 0;

    void incremental(const MarketUpdate& update) {
        now += std::chrono::milliseconds{20};
        kept.received(update, now);
        first_arrival.try_emplace(update.seq, now);
        books.on_incremental(update);
        after();
    }
    void snapshot(const std::vector<MarketUpdate>& updates) {
        for (const MarketUpdate& update : updates) {
            books.on_snapshot(update);
            after();
        }
    }
    std::function<void(const TimeShiftBuffer&)> on_kept;  // after the buffer has looked, each time
    void after() {
        kept.observe(books);
        if (on_kept) {
            on_kept(kept);
        }
        const bool live = books.status() == BookBuilder::Status::live;
        if (live && seen.insert(books.last_seq()).second) {
            const bool run_start = books.last_seq() == kept.runs().back().first;
            const std::size_t past = kept.updates_to_apply(books.last_seq()).value_or(0);
            if (run_start || past < 3 || past + 3 > TimeShiftBuffer::kCheckpointEvery || check(books.last_seq())) {
                compare(copy_of(books));
            }
        } else if (!live && was_live) {
            // A run's last update: the books, leaving live (a gap, or an update they can't apply),
            // are left as they were after it.
            compare(copy_of(books));
        }
        was_live = live;
    }
    std::function<void(const BookBuilder&)> also;  // given the live books, at each compare
    void compare(const Moment& live) {
        if (also) {
            also(books);
        }
        const auto moment = kept.moment(live.seq);
        ASSERT_TRUE(moment) << "update " << live.seq << ": " << moment.error();
        expect_same(*moment, live);
        ++compared;
    }
};

// A feed as a viewer gets it from an exchange it joins late, losing and swapping updates now and
// then, with a snapshot every 250 updates: the same every time. 1,500 updates have each fault once:
// the late join, a loss (at 797) and a swap (at 1,200).
inline void feed_with_faults(Viewer& viewer, int updates = 4000) {
    Exchange exchange{20261026};
    std::vector<MarketUpdate> held;  // updates the feed delivers late
    for (int i = 1; i <= updates; ++i) {
        const MarketUpdate update = exchange.next();
        const bool joined = i > 40;  // the viewer joins late: it must recover first
        if (!joined || (i % 797 == 0)) {
            // Not received: before joining, and now and then, lost.
        } else if (i % 1200 == 0) {
            held.push_back(update);  // arrives after the next: swapped
        } else {
            viewer.incremental(update);
            for (const MarketUpdate& late : held) {
                viewer.incremental(late);
            }
            held.clear();
        }
        if (i % 250 == 0 && joined) {
            viewer.snapshot(exchange.snapshot());
        }
    }
}

}  // namespace fillscope::test
