// A viewer's live books and what it keeps, given a simulated exchange's market data
// (simulated_exchange.h) with faults, for the tests of what the viewer keeps (time_shift_test.cpp,
// kept_file_test.cpp).
#pragma once

#include "feed/simulated_exchange.h"
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
