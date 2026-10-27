// What the viewer keeps of the market data (feed/time_shift.h): the books after every update kept,
// exactly as the live books were then, through a feed that starts late, loses updates, and swaps
// two, recovering from snapshots; every update that wasn't kept refused, saying why; arrival times;
// checkpoints keeping any moment within kCheckpointEvery updates; and a replay held the same way.
#include "feed/time_shift.h"
#include "simulated_feed.h"

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

using namespace fillscope::test;
using Clock = TimeShiftBuffer::Clock;

TEST(TimeShiftBuffer, KeepsTheBooksAfterEveryUpdateThroughGapsAndRecoveries) {
    Viewer viewer;
    // Every moment the live books were in is kept (below); each is compared there and then if it is
    // near a run's edges or a checkpoint, where a mistake would be, and one in 23 elsewhere (all of
    // them would rebuild thousands of moments, each from its checkpoint).
    viewer.check = [](std::uint64_t seq) { return seq % 23 == 0; };
    feed_with_faults(viewer);
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

// The books as of a past update, rebuilt from what was kept and the books at a later one, are the
// live books as they were then: their books, last trades and recoveries; and every order's history
// then is the later books' up to that update, which the rebuilt books don't copy.
// The feed is played twice: the first time to the end, for what was kept and the later books; the
// second, comparing at the updates the first rebuilds.
TEST(BookBuilderAsOf, IsTheLiveBooksAsTheyWereThen) {
    Viewer first;
    first.check = [](std::uint64_t) { return false; };  // compared in the test above; here, played for what it keeps
    feed_with_faults(first);
    // Every order the feed made, with its ticker (each id is one ticker's), found once: its ids run to
    // about 1,100, and none beyond kIds has a history.
    constexpr std::uint64_t kIds = 1300;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> orders;
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        for (std::uint64_t order_id = 1; order_id <= kIds + 200; ++order_id) {
            if (!first.books.history(ticker, order_id).empty()) {
                ASSERT_LE(order_id, kIds) << "the feed's orders go beyond those compared";
                orders.emplace_back(ticker, order_id);
            }
        }
    }
    ASSERT_GT(orders.size(), 1000u);
    Viewer again;
    again.check = [](std::uint64_t seq) { return seq % 251 == 0; };
    std::size_t rebuilt = 0;
    again.also = [&](const BookBuilder& then) {
        // At each run's start, where the books were rebuilt from a snapshot, and one update in 251:
        // each comparison looks up thousands of histories.
        const bool run_start = std::ranges::any_of(first.kept.runs(), [&](const auto& run) { return run.first == then.last_seq(); });
        if (!run_start && then.last_seq() % 251 != 0) {
            return;
        }
        const Moment moment = first.kept.moment(then.last_seq()).value();
        const BookBuilder as_of = BookBuilder::as_of(moment.seq, moment.books, moment.last_trades, first.books);
        ASSERT_EQ(as_of.status(), BookBuilder::Status::live);
        ASSERT_EQ(as_of.last_seq(), then.last_seq());
        EXPECT_TRUE(std::ranges::equal(as_of.recoveries(), then.recoveries())) << "update " << then.last_seq();
        for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
            EXPECT_EQ(as_of.book(ticker).orders(), then.book(ticker).orders());
            EXPECT_EQ(as_of.last_trade(ticker), then.last_trade(ticker));
        }
        for (const auto& [ticker, order_id] : orders) {
            EXPECT_TRUE(std::ranges::equal(first.books.history(ticker, order_id, then.last_seq()), then.history(ticker, order_id)))
                << "update " << then.last_seq() << ", ticker " << ticker << ", order " << order_id;
            EXPECT_TRUE(as_of.history(ticker, order_id).empty());
        }
        ++rebuilt;
    };
    feed_with_faults(again);
    EXPECT_GT(rebuilt, 15u);
    // And given the next update, it goes on as the live books did: from the middle of the first run.
    const TimeShiftBuffer::Run& run = first.kept.runs().front();
    ASSERT_GT(run.last, run.first + 2);
    const std::uint64_t seq = (run.first + run.last) / 2;
    const Moment moment = first.kept.moment(seq).value();
    BookBuilder as_of = BookBuilder::as_of(seq, moment.books, moment.last_trades, first.books);
    as_of.on_incremental(*first.kept.update(seq + 1));
    EXPECT_EQ(as_of.last_seq(), seq + 1);
    const Moment next = first.kept.moment(seq + 1).value();
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        EXPECT_EQ(as_of.book(ticker).orders(), next.books[ticker].orders());
    }
}

TEST(TimeShiftBuffer, StepsFromUpdateToUpdateAcrossGaps) {
    Viewer viewer;
    viewer.check = [](std::uint64_t) { return false; };  // compared elsewhere; here, only the steps
    feed_with_faults(viewer);
    const auto& runs = viewer.kept.runs();
    ASSERT_GE(runs.size(), 2u);
    EXPECT_EQ(viewer.kept.next(runs[0].first), runs[0].first + 1);
    EXPECT_EQ(viewer.kept.next(runs[0].last), runs[1].first);  // across the gap
    EXPECT_FALSE(viewer.kept.next(runs.back().last));          // the newest
    EXPECT_FALSE(viewer.kept.next(runs[0].last + 1));          // in the gap
    EXPECT_EQ(viewer.kept.nearest_shown(runs[0].last + 1), runs[1].first);
    EXPECT_EQ(viewer.kept.nearest_shown(runs[0].first + 1), runs[0].first + 1);
    EXPECT_EQ(viewer.kept.nearest_shown(0), runs[0].first);
    EXPECT_EQ(viewer.kept.nearest_shown(runs.back().last + 100), runs.back().last);
    EXPECT_EQ(viewer.kept.update(runs[0].first), nullptr);  // rebuilt from a snapshot
    ASSERT_NE(viewer.kept.update(runs[0].first + 1), nullptr);
    EXPECT_EQ(viewer.kept.update(runs[0].first + 1)->seq, runs[0].first + 1);
}

}  // namespace
}  // namespace fillscope
