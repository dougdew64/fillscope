// What the viewer keeps of the market data (feed/time_shift.h): the books after every update kept,
// exactly as the live books were then, through a feed that starts late, loses updates, and swaps
// two, recovering from snapshots; every update that wasn't kept refused, saying why; arrival times;
// checkpoints keeping any moment within kCheckpointEvery updates; and a replay held the same way.
#include "feed/time_shift.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace fillscope {
namespace {

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

Moment copy_of(const BookBuilder& books) {
    Moment moment;
    moment.seq = books.last_seq();
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        moment.books[ticker] = books.book(ticker);
        moment.last_trades[ticker] = books.last_trade(ticker);
    }
    return moment;
}

void expect_same(const Moment& kept, const Moment& live) {
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
    void after() {
        kept.observe(books);
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
    void compare(const Moment& live) {
        const auto moment = kept.moment(live.seq);
        ASSERT_TRUE(moment) << "update " << live.seq << ": " << moment.error();
        expect_same(*moment, live);
        ++compared;
    }
};

TEST(TimeShiftBuffer, KeepsTheBooksAfterEveryUpdateThroughGapsAndRecoveries) {
    Exchange exchange{20261026};
    Viewer viewer;
    // Every moment the live books were in is kept (below); each is compared there and then if it is
    // near a run's edges or a checkpoint, where a mistake would be, and one in 23 elsewhere (all of
    // them would rebuild thousands of moments, each from its checkpoint).
    viewer.check = [](std::uint64_t seq) { return seq % 23 == 0; };
    std::vector<MarketUpdate> held;  // updates the feed delivers late
    for (int i = 1; i <= 4000; ++i) {
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
    ASSERT_GE(viewer.books.recoveries().size(), 4u);  // the late join, the losses, the swaps
    viewer.compare(copy_of(viewer.books));             // the newest
    const auto& runs = viewer.kept.runs();
    ASSERT_EQ(runs.size(), viewer.books.recoveries().size());  // each run starts at a recovery
    EXPECT_EQ(viewer.kept.newest(), viewer.books.last_seq());

    for (const std::uint64_t seq : viewer.seen) {
        EXPECT_TRUE(viewer.kept.updates_to_apply(seq)) << "update " << seq << " was live, and isn't kept";
    }
    EXPECT_GT(viewer.compared, viewer.seen.size() / 23);
    // Every update from the first kept to the newest is in a run, or in a gap, and refused, saying so.
    std::size_t refused = 0;
    for (std::uint64_t seq = runs.front().first; seq <= runs.back().last; ++seq) {
        const bool in_a_run = std::ranges::any_of(runs, [&](const auto& run) { return run.first <= seq && seq <= run.last; });
        EXPECT_EQ(viewer.seen.contains(seq), in_a_run) << "update " << seq;
        if (!in_a_run) {
            ++refused;
            const auto kept = viewer.kept.moment(seq);
            ASSERT_FALSE(kept) << "update " << seq;
            EXPECT_NE(kept.error().find(std::format("update {} is in a gap: after update ", seq)), std::string::npos) << kept.error();
        }
    }
    EXPECT_GT(refused, 0u);
    // And the arrival times: each update kept, applied one by one, when it first arrived.
    for (const TimeShiftBuffer::Run& run : runs) {
        EXPECT_FALSE(viewer.kept.arrived(run.first));  // rebuilt from a snapshot, not arrived as such
        for (std::uint64_t seq = run.first + 1; seq <= run.last; ++seq) {
            EXPECT_EQ(viewer.kept.arrived(seq), viewer.first_arrival.at(seq)) << "update " << seq;
        }
    }
}

TEST(TimeShiftBuffer, FromUpdateOneTheFirstRunStartsBeforeIt) {
    Exchange exchange{7};
    Viewer viewer;
    for (int i = 0; i < 50; ++i) {
        viewer.incremental(exchange.next());
    }
    EXPECT_EQ(viewer.kept.runs(), (std::vector<TimeShiftBuffer::Run>{{.first = 0, .last = 50}}));
    const auto empty = viewer.kept.moment(0);
    ASSERT_TRUE(empty);
    for (const Book& book : empty->books) {
        EXPECT_EQ(book.order_count(), 0u);
    }
    EXPECT_EQ(viewer.compared, 50u);  // every update, as it was live
    EXPECT_EQ(viewer.kept.kept(), 50u);
}

TEST(TimeShiftBuffer, SaysWhyAnUpdateCantBeShown) {
    TimeShiftBuffer nothing;
    EXPECT_EQ(nothing.moment(1).error(), "nothing has been kept yet");
    EXPECT_FALSE(nothing.newest());

    Exchange exchange{3};
    Viewer viewer;
    for (int i = 1; i <= 20; ++i) {
        const MarketUpdate update = exchange.next();
        if (i > 10) {
            viewer.incremental(update);  // joined at 11: recovers from the snapshot as of 20
        }
    }
    viewer.snapshot(exchange.snapshot());
    for (int i = 0; i < 5; ++i) {
        viewer.incremental(exchange.next());
    }
    EXPECT_EQ(viewer.kept.runs(), (std::vector<TimeShiftBuffer::Run>{{.first = 20, .last = 25}}));
    EXPECT_EQ(viewer.kept.moment(19).error(), "the books are kept from update 20 on");
    EXPECT_EQ(viewer.kept.moment(26).error(), "update 26 hasn't arrived; the newest kept is 25");
    EXPECT_TRUE(viewer.kept.moment(20));
}

TEST(TimeShiftBuffer, AnyMomentIsACheckpointAndFewerThanItsIntervalOfUpdates) {
    Exchange exchange{11};
    Viewer viewer;
    viewer.check = [](std::uint64_t) { return false; };  // around checkpoints only
    const std::size_t count = 3 * TimeShiftBuffer::kCheckpointEvery + 17;
    for (std::size_t i = 0; i < count; ++i) {
        viewer.incremental(exchange.next());
    }
    std::size_t most = 0;
    for (std::uint64_t seq = 0; seq <= count; ++seq) {
        most = std::max(most, viewer.kept.updates_to_apply(seq).value());
    }
    EXPECT_EQ(most, TimeShiftBuffer::kCheckpointEvery - 1);
    // Right after a checkpoint, nothing to apply.
    EXPECT_EQ(viewer.kept.updates_to_apply(TimeShiftBuffer::kCheckpointEvery), 0u);
    // And the moments around each checkpoint were exact, compared as they were live: those 0 to 2
    // updates past it and 1 to 2 short of the next, over 3 checkpoints after the run's start and 17
    // updates past the last: 4 (there is no update 0 to compare) + 5 + 5 + 3.
    EXPECT_EQ(viewer.compared, 17u);
}

TEST(TimeShiftBuffer, AReplayIsHeldTheSameWayWithNoTimes) {
    Exchange exchange{5};
    std::vector<MarketUpdate> updates;
    for (std::size_t i = 0; i < 2 * TimeShiftBuffer::kCheckpointEvery + 3; ++i) {
        updates.push_back(exchange.next());
    }
    const TimeShiftBuffer replay = TimeShiftBuffer::from_replay(updates);
    EXPECT_EQ(replay.runs(), (std::vector<TimeShiftBuffer::Run>{{.first = 0, .last = updates.size()}}));
    // The books after each update, as the live books built them, compared around the checkpoints
    // and one in 13 elsewhere.
    BookBuilder books;
    for (const MarketUpdate& update : updates) {
        books.on_incremental(update);
        const std::size_t past = replay.updates_to_apply(update.seq).value();
        if (past < 3 || past + 3 > TimeShiftBuffer::kCheckpointEvery || update.seq % 13 == 0) {
            expect_same(replay.moment(update.seq).value(), copy_of(books));
        }
        EXPECT_FALSE(replay.arrived(update.seq));
    }
    EXPECT_LT(replay.updates_to_apply(updates.size()).value(), TimeShiftBuffer::kCheckpointEvery);

    // A replay's updates stop where the books can't apply one, as the viewer's replay stops there.
    std::vector<MarketUpdate> broken{updates.begin(), updates.begin() + 10};
    broken.push_back({.seq = 11, .type = Type::cancel, .order_id = 999'999, .ticker = 0});  // no such order
    broken.push_back(updates[11]);
    EXPECT_EQ(TimeShiftBuffer::from_replay(broken).newest(), 10u);
}

}  // namespace
}  // namespace fillscope
