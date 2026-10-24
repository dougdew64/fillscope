// The viewer's core, without Dear ImGui: its command line (parse_viewer_args), what the user has
// pointed at and followed (Selection), the context built from the books and the selection
// (make_context, to_json, bar_text), and the file it is published to (ContextPublisher,
// write_atomically). The books themselves are tested in tests/feed/.

#include "viewer/context.h"
#include "viewer/context_publisher.h"
#include "viewer/stop_signals.h"
#include "viewer/viewer_options.h"
#include "viewer/viewer_state.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <vector>

using namespace std::chrono_literals;

namespace fillscope {
namespace {

MarketUpdate add(std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty, std::uint64_t priority) {
    return {.seq = seq, .type = UpdateType::add, .order_id = id, .ticker = 0, .side = side, .price = price, .qty = qty, .priority = priority};
}

MarketUpdate modify(std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty) {
    return {.seq = seq, .type = UpdateType::modify, .order_id = id, .ticker = 0, .side = side, .price = price, .qty = qty, .priority = 0};
}

MarketUpdate cancel(std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price) {
    return {.seq = seq, .type = UpdateType::cancel, .order_id = id, .ticker = 0, .side = side, .price = price, .qty = 0, .priority = 0};
}

// Ticker 0's book: orders 1 and 2 bid 100 (1 first), order 3 bids 99.
ViewerState three_bids() {
    ViewerState state;
    state.books.on_incremental(add(1, 1, Side::buy, 100, 5, 1));
    state.books.on_incremental(add(2, 2, Side::buy, 100, 7, 2));
    state.books.on_incremental(add(3, 3, Side::buy, 99, 4, 1));
    return state;
}

// A snapshot as the exchange sends it, of ticker 0's orders, as of update `last_seq`.
void recover_from_snapshot(BookBuilder& books, std::uint64_t last_seq, const std::vector<MarketUpdate>& orders) {
    std::vector<MarketUpdate> updates{{.seq = 0, .type = UpdateType::snapshot_start, .order_id = last_seq}};
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        updates.push_back({.type = UpdateType::clear, .ticker = ticker});
        if (ticker == 0) {
            updates.insert(updates.end(), orders.begin(), orders.end());
        }
    }
    updates.push_back({.type = UpdateType::snapshot_end, .order_id = last_seq});
    for (std::uint64_t i = 0; i < updates.size(); ++i) {
        updates[i].seq = i;
        books.on_snapshot(updates[i]);
    }
}

const OrderState& pointed_order(const Context& context) { return std::get<OrderState>(context.point->target); }
const LevelState& pointed_level(const Context& context) { return std::get<LevelState>(context.point->target); }

// ---- parse_viewer_args ----

TEST(ParseViewerArgs, DefaultsAndEveryOption) {
    const auto defaults = parse_viewer_args({});
    ASSERT_TRUE(defaults) << defaults.error();
    EXPECT_EQ(defaults->exit_after_seconds, 0);
    EXPECT_EQ(defaults->incremental, kExchangeIncremental);
    EXPECT_EQ(defaults->snapshot, kExchangeSnapshot);
    EXPECT_EQ(defaults->context_file, std::filesystem::path{".fillscope/context.json"});

    EXPECT_FALSE(defaults->replay);

    const std::array args{"--exit-after", "2.5", "--incremental", "239.1.2.3:3001", "--snapshot", "224.0.0.5:3000",
                          "--context-file", "/tmp/c.json"};
    const auto options = parse_viewer_args(args);
    ASSERT_TRUE(options) << options.error();
    EXPECT_EQ(options->exit_after_seconds, 2.5);
    EXPECT_EQ(options->incremental, (Stream{"239.1.2.3", 3001}));
    EXPECT_EQ(options->snapshot, (Stream{"224.0.0.5", 3000}));
    EXPECT_EQ(options->context_file, std::filesystem::path{"/tmp/c.json"});
    EXPECT_FALSE(options->replay);

    // A replay instead of the streams.
    const auto replay = parse_viewer_args(std::array{"--replay", "runs/a.dat", "--context-file", "/tmp/c.json"});
    ASSERT_TRUE(replay) << replay.error();
    EXPECT_EQ(replay->replay, std::filesystem::path{"runs/a.dat"});
    EXPECT_EQ(replay->context_file, std::filesystem::path{"/tmp/c.json"});
}

TEST(ParseViewerArgs, RefusesWhatIsntAnOptionAStreamOrANumberOfSeconds) {
    for (const auto& args : std::vector<std::vector<const char*>>{
             {"--bogus"}, {"--exit-after"}, {"--exit-after", "-1"}, {"--exit-after", "nan"}, {"--exit-after", "inf"},
             {"--exit-after", "2s"}, {"--context-file", ""}, {"--socket", "x"},
             {"--incremental", "239.1.2.3"},          // no port
             {"--incremental", "10.0.0.1:3000"},      // not a multicast address
             {"--incremental", "240.0.0.1:3000"},     // past the multicast range
             {"--incremental", "not.an.address:3000"},
             {"--snapshot", "239.1.2.3:0"}, {"--snapshot", "239.1.2.3:65536"}, {"--snapshot", "239.1.2.3:x"},
             {"--replay"}, {"--replay", ""},
             {"--replay", "a.dat", "--incremental", "239.1.2.3:3001"}, {"--snapshot", "239.1.2.3:3000", "--replay", "a.dat"}}) {
        EXPECT_FALSE(parse_viewer_args(args)) << args.front() << (args.size() > 1 ? std::string{" "} + args[1] : "");
    }
}

// ---- Selection ----

TEST(Selection, EachChangeTakesTheNextNumberFromOneCounter) {
    Selection selection;
    selection.point_at(OrderRef{0, 1});
    EXPECT_EQ(selection.point_seq(), 1u);
    selection.follow(OrderRef{0, 2});
    EXPECT_EQ(selection.follow_seq(), 2u);
    selection.point_at(LevelRef{0, Side::buy, 100});
    EXPECT_EQ(selection.point_seq(), 3u);
    selection.clear_follow();
    EXPECT_EQ(selection.follow_seq(), 4u);
    EXPECT_FALSE(selection.followed());
    selection.clear_point();
    EXPECT_EQ(selection.point_seq(), 5u);
    EXPECT_FALSE(selection.pointed());
}

// ---- make_context ----

TEST(MakeContext, ARestingOrderPointedAtKnowsWhatIsAheadOfItInItsQueue) {
    const ViewerState state = three_bids();
    Selection selection;
    selection.point_at(OrderRef{0, 2});
    const Context context = make_context(state, selection);
    const OrderState& order = pointed_order(context);
    ASSERT_TRUE(order.resting);
    EXPECT_EQ(order.resting->price, 100);
    EXPECT_EQ(order.orders_ahead, 1u);
    EXPECT_EQ(order.qty_ahead, 5u);
    ASSERT_TRUE(order.last_event);
    EXPECT_EQ(order.last_event->seq, 2u);
    EXPECT_EQ(context.point->seq, 1u);

    selection.point_at(OrderRef{0, 1});
    EXPECT_EQ(pointed_order(make_context(state, selection)).orders_ahead, 0u) << "first in its queue";
}

TEST(MakeContext, AnOrderNoLongerRestingSaysHowItLeft) {
    ViewerState state = three_bids();
    state.books.on_incremental(cancel(4, 1, Side::buy, 100));
    Selection selection;
    selection.point_at(OrderRef{0, 1});
    const Context context = make_context(state, selection);  // kept: order refers into it
    const OrderState& order = pointed_order(context);
    EXPECT_FALSE(order.resting);
    ASSERT_TRUE(order.last_event);
    EXPECT_EQ(order.last_event->type, UpdateType::cancel);
    EXPECT_EQ(order.last_event->seq, 4u);
}

// Order 3 is cancelled in plain sight; then a gap (updates 5 and 6 lost), and the snapshot as of 6
// no longer has order 1: it left in the gap, so its last update seen (its add) isn't how it left,
// and the point says so. Order 3 left by its cancel, before the recovery.
TEST(MakeContext, AnOrderThatLeftDuringARecoverySaysSo) {
    ViewerState state = three_bids();
    state.books.on_incremental(cancel(4, 3, Side::buy, 99));
    state.books.on_incremental(add(7, 4, Side::buy, 98, 1, 1));
    recover_from_snapshot(state.books, 6, {add(0, 2, Side::buy, 100, 7, 2)});
    ASSERT_EQ(state.books.status(), BookBuilder::Status::live);
    Selection selection;
    selection.point_at(OrderRef{0, 1});
    const Context context = make_context(state, selection);  // kept: order refers into it
    const OrderState& order = pointed_order(context);
    EXPECT_FALSE(order.resting);
    ASSERT_TRUE(order.last_event);
    EXPECT_EQ(order.last_event->type, UpdateType::add);
    ASSERT_TRUE(order.left_during);
    EXPECT_EQ(order.left_during->after_seq, 4u);
    EXPECT_EQ(order.left_during->snapshot_seq, 6u);
    EXPECT_EQ(bar_text(context).point,
              "Pointing at order 1 of ticker 0: not in the book; it left during a recovery (updates 5 to 6 weren't seen "
              "one by one), after its last update seen: add at update 1");
    const auto json = to_json(context, {}, 1, 0);
    EXPECT_EQ(json["point"]["order"]["leftDuringRecovery"]["snapshotSeq"], 6);

    selection.point_at(OrderRef{0, 3});
    const Context cancelled = make_context(state, selection);
    EXPECT_FALSE(pointed_order(cancelled).left_during);
    EXPECT_EQ(bar_text(cancelled).point, "Pointing at order 3 of ticker 0: not in the book (its last update: cancel at update 4)");
}

TEST(MakeContext, AnOrderNeverSeenHasNeitherAPlaceNorAHistory) {
    Selection selection;
    selection.point_at(OrderRef{0, 99});
    const Context context = make_context(three_bids(), selection);  // kept: order refers into it
    const OrderState& order = pointed_order(context);
    EXPECT_FALSE(order.resting);
    EXPECT_FALSE(order.last_event);
}

TEST(MakeContext, ALevelPointedAtListsItsOrdersInTheOrderTheyWillFill) {
    Selection selection;
    selection.point_at(LevelRef{0, Side::buy, 100});
    const Context context = make_context(three_bids(), selection);  // kept: level refers into it
    const LevelState& level = pointed_level(context);
    EXPECT_TRUE(level.present);
    EXPECT_EQ(level.qty, 12u);
    EXPECT_EQ(level.order_count, 2u);
    ASSERT_EQ(level.orders.size(), 2u);
    EXPECT_EQ(level.orders[0].order_id, 1u);

    selection.point_at(LevelRef{0, Side::sell, 100});
    EXPECT_FALSE(pointed_level(make_context(three_bids(), selection)).present) << "no asks at 100";
}

TEST(MakeContext, ALevelsListIsCappedButItsCountIsExact) {
    ViewerState state;
    for (std::uint64_t i = 1; i <= kMaxLevelOrders + 5; ++i) {
        state.books.on_incremental(add(i, i, Side::sell, 105, 1, i));
    }
    Selection selection;
    selection.point_at(LevelRef{0, Side::sell, 105});
    const Context context = make_context(state, selection);  // kept: level refers into it
    const LevelState& level = pointed_level(context);
    EXPECT_EQ(level.orders.size(), kMaxLevelOrders);
    EXPECT_EQ(level.order_count, kMaxLevelOrders + 5);
    EXPECT_TRUE(level.truncated());
}

TEST(MakeContext, AFollowedOrderHasEveryEventInOrder) {
    ViewerState state = three_bids();
    state.books.on_incremental(modify(4, 1, Side::buy, 100, 3));
    state.books.on_incremental(cancel(5, 1, Side::buy, 100));
    Selection selection;
    selection.follow(OrderRef{0, 1});
    const Context context = make_context(state, selection);
    ASSERT_TRUE(context.follow);
    EXPECT_EQ(context.follow->event_count, 3u);
    ASSERT_EQ(context.follow->events.size(), 3u);
    EXPECT_EQ(context.follow->events[1].qty, 3u);
    EXPECT_EQ(context.follow->events[2].type, UpdateType::cancel);
    EXPECT_FALSE(context.follow->order.resting);
    EXPECT_TRUE(context.follow->gaps.empty());
}

TEST(MakeContext, AFollowListIsCappedButItsCountIsExact) {
    ViewerState state;
    state.books.on_incremental(add(1, 1, Side::buy, 100, 5'000, 1));
    for (std::uint64_t seq = 2; seq <= kMaxFollowedEvents + 10; ++seq) {
        state.books.on_incremental(modify(seq, 1, Side::buy, 100, static_cast<std::uint32_t>(5'000 - seq)));
    }
    Selection selection;
    selection.follow(OrderRef{0, 1});
    const Context context = make_context(state, selection);
    EXPECT_EQ(context.follow->events.size(), kMaxFollowedEvents);
    EXPECT_EQ(context.follow->event_count, kMaxFollowedEvents + 10);
    EXPECT_TRUE(context.follow->truncated());
}

TEST(MakeContext, AFollowedOrdersGapsAreTheRecoveriesSinceItWasFirstSeen) {
    ViewerState state = three_bids();
    state.books.on_incremental(add(6, 6, Side::buy, 98, 1, 1));  // 4 and 5 missed
    recover_from_snapshot(state.books, 5, {add(0, 1, Side::buy, 100, 2, 1), add(0, 3, Side::buy, 99, 4, 1)});
    ASSERT_EQ(state.books.status(), BookBuilder::Status::live);
    Selection selection;
    selection.follow(OrderRef{0, 1});
    Context context = make_context(state, selection);
    ASSERT_EQ(context.follow->gaps.size(), 1u);
    EXPECT_EQ(context.follow->gaps[0].after_seq, 3u);
    EXPECT_EQ(context.follow->gaps[0].snapshot_seq, 5u);
    EXPECT_TRUE(context.follow->events.back().from_snapshot) << "its quantity changed while unseen";

    selection.follow(OrderRef{0, 6});
    context = make_context(state, selection);
    EXPECT_TRUE(context.follow->gaps.empty()) << "first seen after the recovery";
}

TEST(MakeContext, TheFeedSaysWhereTheBooksAreAndWhichTickerIsShown) {
    ViewerState state = three_bids();
    state.shown_ticker = 5;
    state.feed_error = "a datagram of 10 bytes isn't a whole number of 42-byte updates";
    const Context::Feed feed = make_context(state, Selection{}).feed;
    EXPECT_EQ(feed.status, BookBuilder::Status::live);
    EXPECT_EQ(feed.last_seq, 3u);
    EXPECT_EQ(feed.updates_received, 3u);
    EXPECT_EQ(feed.shown_ticker, 5u);
    EXPECT_EQ(feed.error, state.feed_error);
}

TEST(MakeContext, OnlyTheLatestRecoveriesAreListedButAllAreCounted) {
    ViewerState state;
    std::uint64_t seq = 0;
    for (std::size_t i = 0; i < kMaxListedRecoveries + 3; ++i) {
        seq += 2;  // one update missed each time
        state.books.on_incremental(add(seq, seq, Side::buy, 100, 1, seq));
        recover_from_snapshot(state.books, seq - 1, {});
        ASSERT_EQ(state.books.status(), BookBuilder::Status::live);
    }
    const Context::Feed feed = make_context(state, Selection{}).feed;
    EXPECT_EQ(feed.recovery_count, kMaxListedRecoveries + 3);
    ASSERT_EQ(feed.latest_recoveries.size(), kMaxListedRecoveries);
    EXPECT_EQ(feed.latest_recoveries.back().snapshot_seq, seq - 1) << "the latest, last";
}

// ---- A replay ----

// three_bids(), as a replay of 4 updates (the fourth cancels order 1), at update `at`.
ViewerState replay_at(std::uint64_t at) {
    ViewerState state;
    state.replay = Replay{.file = "/runs/a.dat", .recording = "a.rec",
                          .updates = {add(1, 1, Side::buy, 100, 5, 1), add(2, 2, Side::buy, 100, 7, 2),
                                      add(3, 3, Side::buy, 99, 4, 1), cancel(4, 1, Side::buy, 100)}};
    for (std::uint64_t seq = 1; seq <= at; ++seq) {
        state.books.on_incremental(state.replay->updates[seq - 1]);
    }
    return state;
}

TEST(MakeContext, InAReplayPointAndFollowHaveTheAddressOfTheStateTheyDescribe) {
    Selection selection;
    selection.point_at(OrderRef{0, 1});
    selection.follow(OrderRef{0, 2});
    const Context context = make_context(replay_at(3), selection);
    const ReplayAddress expected{.market_data_file = "/runs/a.dat", .recording = "a.rec", .seq = 3};
    EXPECT_EQ(context.point->replay, expected);
    EXPECT_EQ(context.follow->replay, expected);
    EXPECT_EQ(context.feed.replay, (ReplayAddress{.market_data_file = "/runs/a.dat", .recording = "a.rec", .seq = 4}))
        << "the background's seq is the replay's length";
    EXPECT_TRUE(std::get<OrderState>(context.point->target).resting) << "order 1 rests after update 3";
    EXPECT_FALSE(std::get<OrderState>(make_context(replay_at(4), selection).point->target).resting) << "and not after 4";
}

TEST(MakeContext, LiveThereIsNoReplayAddress) {
    Selection selection;
    selection.point_at(OrderRef{0, 1});
    const Context context = make_context(three_bids(), selection);
    EXPECT_FALSE(context.point->replay);
    EXPECT_FALSE(context.feed.replay);
}

TEST(ContextJson, AReplaySaysWhichAndEveryAddressRebuildsItsState) {
    Selection selection;
    selection.point_at(OrderRef{0, 2});
    selection.follow(OrderRef{0, 2});
    const auto json = nlohmann::json::parse(to_json(make_context(replay_at(2), selection), Writer{}, 1, 0).dump());
    EXPECT_EQ(json["background"]["replay"]["marketDataFile"], "/runs/a.dat");
    EXPECT_EQ(json["background"]["replay"]["recording"], "a.rec");
    EXPECT_EQ(json["background"]["replay"]["updates"], 4);
    EXPECT_TRUE(json["background"]["noReplayAddress"].is_null());
    for (const auto* section : {"point", "follow"}) {
        EXPECT_EQ(json[section]["replayAddress"]["marketDataFile"], "/runs/a.dat") << section;
        EXPECT_EQ(json[section]["replayAddress"]["recording"], "a.rec") << section;
        EXPECT_EQ(json[section]["replayAddress"]["seq"], 2) << section;
    }

    const auto live = nlohmann::json::parse(to_json(make_context(three_bids(), selection), Writer{}, 1, 0).dump());
    EXPECT_TRUE(live["background"]["replay"].is_null());
    EXPECT_EQ(live["background"]["noReplayAddress"], kNoReplayAddress);
    EXPECT_TRUE(live["point"]["replayAddress"].is_null());
    EXPECT_TRUE(live["follow"]["replayAddress"].is_null());
}

TEST(ContextBar, SaysWhereInTheReplayTheBooksAre) {
    EXPECT_EQ(bar_text(make_context(replay_at(3), Selection{})).background,
              "replay of /runs/a.dat at update 3 of 4 · showing ticker 0");
}

// ---- to_json ----

TEST(ContextJson, EmptySectionsAreNullAndEveryFieldIsThere) {
    const auto json = nlohmann::json::parse(to_json(make_context(ViewerState{}, Selection{}), Writer{}, 1, 0).dump());
    EXPECT_EQ(json["version"], 2);
    EXPECT_TRUE(json["point"].is_null());
    EXPECT_TRUE(json["follow"].is_null());
    const auto& feed = json["background"]["feed"];
    EXPECT_EQ(feed["status"], "waiting");
    EXPECT_EQ(feed["lastSeq"], 0);
    EXPECT_EQ(feed["updatesReceived"], 0);
    EXPECT_TRUE(feed["recoveringBecause"].is_null());
    EXPECT_EQ(feed["recoveryCount"], 0);
    EXPECT_TRUE(feed["error"].is_null());
    EXPECT_EQ(feed["latestRecoveries"].size(), 0u);
    EXPECT_EQ(json["background"]["shownTicker"], 0);
    EXPECT_EQ(json["background"]["noReplayAddress"], kNoReplayAddress);
}

TEST(ContextJson, SaysWhoWroteItAndWhetherItIsStillRunning) {
    const auto json = nlohmann::json::parse(to_json(Context{}, Writer{.pid = 7, .started_at_ms = 11, .running = false}, 3, 99).dump());
    EXPECT_EQ(json["seq"], 3);
    EXPECT_EQ(json["writtenAtMs"], 99);
    EXPECT_EQ(json["writer"]["program"], "viewer");
    EXPECT_EQ(json["writer"]["pid"], 7);
    EXPECT_EQ(json["writer"]["startedAtMs"], 11);
    EXPECT_EQ(json["writer"]["running"], false);
}

TEST(ContextJson, APointedOrderCarriesItsPlaceAndItsLastEvent) {
    Selection selection;
    selection.point_at(OrderRef{0, 2});
    const auto json = nlohmann::json::parse(to_json(make_context(three_bids(), selection), Writer{}, 1, 0).dump());
    const auto& point = json["point"];
    EXPECT_EQ(point["seq"], 1);
    EXPECT_EQ(point["asOfSeq"], 3);
    const auto& order = point["order"];
    EXPECT_EQ(order["ticker"], 0);
    EXPECT_EQ(order["orderId"], 2);
    EXPECT_EQ(order["resting"]["side"], "buy");
    EXPECT_EQ(order["resting"]["price"], 100);
    EXPECT_EQ(order["resting"]["qty"], 7);
    EXPECT_EQ(order["resting"]["priority"], 2);
    EXPECT_EQ(order["resting"]["ordersAhead"], 1);
    EXPECT_EQ(order["resting"]["qtyAhead"], 5);
    EXPECT_EQ(order["lastEvent"]["type"], "add");
    EXPECT_EQ(order["lastEvent"]["seq"], 2);
    EXPECT_EQ(order["lastEvent"]["fromSnapshot"], false);
    EXPECT_FALSE(point.contains("level"));
}

TEST(ContextJson, APointedLevelCarriesItsOrders) {
    Selection selection;
    selection.point_at(LevelRef{0, Side::buy, 100});
    const auto json = nlohmann::json::parse(to_json(make_context(three_bids(), selection), Writer{}, 1, 0).dump());
    const auto& level = json["point"]["level"];
    EXPECT_EQ(level["side"], "buy");
    EXPECT_EQ(level["price"], 100);
    EXPECT_EQ(level["present"], true);
    EXPECT_EQ(level["qty"], 12);
    EXPECT_EQ(level["orderCount"], 2);
    EXPECT_EQ(level["orders"][1]["orderId"], 2);
    EXPECT_EQ(level["ordersTruncated"], false);
    EXPECT_FALSE(json["point"].contains("order"));
}

TEST(ContextJson, AFollowedOrderCarriesItsEventsAndGaps) {
    ViewerState state = three_bids();
    state.books.on_incremental(cancel(4, 1, Side::buy, 100));
    Selection selection;
    selection.follow(OrderRef{0, 1});
    const auto json = nlohmann::json::parse(to_json(make_context(state, selection), Writer{}, 1, 0).dump());
    const auto& follow = json["follow"];
    EXPECT_EQ(follow["asOfSeq"], 4);
    EXPECT_EQ(follow["order"]["orderId"], 1);
    EXPECT_TRUE(follow["order"]["resting"].is_null());
    EXPECT_EQ(follow["eventCount"], 2);
    EXPECT_EQ(follow["events"][1]["type"], "cancel");
    EXPECT_EQ(follow["events"][1]["seq"], 4);
    EXPECT_EQ(follow["eventsTruncated"], false);
    EXPECT_EQ(follow["gaps"].size(), 0u);
}

TEST(ContextJson, ARecoveringFeedSaysWhyAndListsItsRecoveries) {
    ViewerState state = three_bids();
    state.books.on_incremental(add(5, 5, Side::buy, 90, 1, 1));  // 4 missed
    auto json = nlohmann::json::parse(to_json(make_context(state, Selection{}), Writer{}, 1, 0).dump());
    EXPECT_EQ(json["background"]["feed"]["status"], "recovering");
    EXPECT_EQ(json["background"]["feed"]["recoveringBecause"], "update 5 arrived when 4 was next");
    recover_from_snapshot(state.books, 4, {});
    json = nlohmann::json::parse(to_json(make_context(state, Selection{}), Writer{}, 1, 0).dump());
    EXPECT_EQ(json["background"]["feed"]["status"], "live");
    EXPECT_EQ(json["background"]["feed"]["recoveryCount"], 1);
    EXPECT_EQ(json["background"]["feed"]["latestRecoveries"][0]["afterSeq"], 3);
    EXPECT_EQ(json["background"]["feed"]["latestRecoveries"][0]["snapshotSeq"], 4);
    EXPECT_EQ(json["background"]["feed"]["latestRecoveries"][0]["reason"], "update 5 arrived when 4 was next");
}

// ---- bar_text ----

TEST(ContextBar, SaysWhereTheFeedIsAndWhichTickerIsShown) {
    EXPECT_EQ(bar_text(make_context(ViewerState{}, Selection{})).background, "waiting for the exchange's market data · showing ticker 0");
    ViewerState state = three_bids();
    state.shown_ticker = 2;
    EXPECT_EQ(bar_text(make_context(state, Selection{})).background, "exchange live at update 3 · showing ticker 2");
    state.books.on_incremental(add(5, 5, Side::buy, 90, 1, 1));
    EXPECT_EQ(bar_text(make_context(state, Selection{})).background,
              "exchange: recovering, the books are as of update 3 (update 5 arrived when 4 was next) · showing ticker 2");
}

TEST(ContextBar, CountsRecoveriesAndShowsTheLastError) {
    ViewerState state = three_bids();
    state.books.on_incremental(add(5, 5, Side::buy, 90, 1, 1));
    recover_from_snapshot(state.books, 4, {});
    state.feed_error = "bad datagram";
    EXPECT_EQ(bar_text(make_context(state, Selection{})).background,
              "exchange live at update 5 · showing ticker 0 · 1 recovery · last error: bad datagram");
}

TEST(ContextBar, SaysWhereAPointedOrderIsOrHowItLeft) {
    ViewerState state = three_bids();
    Selection selection;
    selection.point_at(OrderRef{0, 2});
    EXPECT_EQ(bar_text(make_context(state, selection)).point,
              "Pointing at order 2 of ticker 0: buy 7 @ 100, 1 order (5) ahead of it in its queue");
    state.books.on_incremental(cancel(4, 2, Side::buy, 100));
    EXPECT_EQ(bar_text(make_context(state, selection)).point,
              "Pointing at order 2 of ticker 0: not in the book (its last update: cancel at update 4)");
    selection.point_at(OrderRef{0, 99});
    EXPECT_EQ(bar_text(make_context(state, selection)).point, "Pointing at order 99 of ticker 0: never seen");
}

TEST(ContextBar, SaysWhatAPointedLevelHolds) {
    Selection selection;
    selection.point_at(LevelRef{0, Side::buy, 100});
    EXPECT_EQ(bar_text(make_context(three_bids(), selection)).point, "Pointing at buy 100 of ticker 0: 12 in 2 orders");
    selection.point_at(LevelRef{0, Side::sell, 101});
    EXPECT_EQ(bar_text(make_context(three_bids(), selection)).point,
              "Pointing at sell 101 of ticker 0: no orders at that price now");
}

TEST(ContextBar, SaysWhatAFollowedOrderHasDoneAndAnyGaps) {
    ViewerState state = three_bids();
    Selection selection;
    selection.follow(OrderRef{0, 3});
    EXPECT_EQ(bar_text(make_context(state, selection)).follow,
              "Following order 3 of ticker 0: 1 event, buy 4 @ 99, 0 orders (0) ahead of it in its queue");
    state.books.on_incremental(add(5, 5, Side::buy, 90, 1, 1));
    recover_from_snapshot(state.books, 4, {add(0, 3, Side::buy, 99, 4, 1)});
    EXPECT_EQ(bar_text(make_context(state, selection)).follow,
              "Following order 3 of ticker 0: 2 events, buy 4 @ 99, 0 orders (0) ahead of it in its queue · "
              "1 recovery may hide some of its events");
}

TEST(ContextBar, ShowsNoPointOrFollowLineWhenThereIsNone) {
    const ContextBarText text = bar_text(make_context(three_bids(), Selection{}));
    EXPECT_FALSE(text.point);
    EXPECT_FALSE(text.follow);
}

// ---- ContextPublisher and write_atomically ----

// A fresh directory for each test, removed afterwards.
class Publisher : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               std::format("fillscope-context-{}-{}", ::getpid(),
                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] std::filesystem::path file() const { return dir_ / ".fillscope" / "context.json"; }

    [[nodiscard]] nlohmann::json read_file() const {
        std::ifstream in{file()};
        return nlohmann::json::parse(in);
    }

    [[nodiscard]] std::ptrdiff_t entries_in(const std::filesystem::path& directory) const {
        return std::ranges::distance(std::filesystem::directory_iterator{directory}, std::filesystem::directory_iterator{});
    }

    // A path every write to fails: a directory can't be created inside a regular file.
    [[nodiscard]] std::filesystem::path unwritable() const {
        std::filesystem::create_directories(dir_);
        std::ofstream{dir_ / "not-a-directory"} << "x";
        return dir_ / "not-a-directory" / "context.json";
    }

    std::filesystem::path dir_;
    const ContextPublisher::Clock::time_point t0_{};
};

// Ticker 0's book, pointing at one of its orders.
Context pointing_at(std::uint64_t order_id) {
    Selection selection;
    selection.point_at(OrderRef{0, order_id});
    return make_context(three_bids(), selection);
}

TEST_F(Publisher, AUserActionIsWrittenAtOnce) {
    ContextPublisher publisher{file(), 100ms};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_ + 1ms);  // well inside the interval
    EXPECT_EQ(publisher.seq(), 2u);
    EXPECT_EQ(read_file()["point"]["order"]["orderId"], 2);
}

TEST_F(Publisher, ABackgroundChangeWaitsForTheInterval) {
    ContextPublisher publisher{file(), 100ms};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    ViewerState more = three_bids();
    more.books.on_incremental(add(4, 4, Side::buy, 97, 1, 1));
    const Context later = make_context(more, Selection{});

    publisher.update(later, PublishTrigger::background, t0_ + 50ms);
    EXPECT_EQ(publisher.seq(), 1u) << "held back: too soon";
    EXPECT_NE(publisher.published(), later);

    publisher.update(later, PublishTrigger::background, t0_ + 100ms);
    EXPECT_EQ(publisher.seq(), 2u) << "written once the interval passed";
    EXPECT_EQ(publisher.published(), later);
}

TEST_F(Publisher, AnUnchangedContextIsNotRewritten) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_ + 1s);
    EXPECT_EQ(publisher.seq(), 1u);
}

TEST_F(Publisher, TheFileHoldsExactlyWhatPublishedSays) {
    ContextPublisher publisher{file()};
    const auto wall_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    const auto before = wall_ms();
    publisher.update(pointing_at(3), PublishTrigger::user_action, t0_);
    const auto after = wall_ms();

    const nlohmann::json written = read_file();
    const auto written_at = written["writtenAtMs"].get<std::int64_t>();
    EXPECT_GE(written_at, before);
    EXPECT_LE(written_at, after);
    const auto expected = to_json(*publisher.published(), publisher.writer(), publisher.seq(), written_at);
    EXPECT_EQ(written, nlohmann::json::parse(expected.dump()));
    EXPECT_EQ(written["writer"]["pid"], ::getpid());
    EXPECT_EQ(written["writer"]["running"], true);
}

TEST_F(Publisher, FinishSaysTheWriterIsNoLongerRunning) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    publisher.finish(pointing_at(1));
    EXPECT_EQ(read_file()["writer"]["running"], false);
    EXPECT_EQ(publisher.seq(), 2u);
}

TEST_F(Publisher, NoTemporaryFileIsLeftBehind) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_);
    EXPECT_EQ(entries_in(file().parent_path()), 1);
}

TEST_F(Publisher, AFailedWriteKeepsWhatTheFileHoldsAndSaysWhy) {
    ContextPublisher publisher{unwritable()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    EXPECT_FALSE(publisher.published()) << "nothing was written, so nothing is published";
    ASSERT_TRUE(publisher.error());
    EXPECT_NE(publisher.error()->find("can't create"), std::string::npos) << *publisher.error();
    EXPECT_NE(publisher.error()->find("Not a directory"), std::string::npos) << "the reason, not just the fact";
    EXPECT_EQ(publisher.seq(), 0u);
}

TEST_F(Publisher, AFailedWriteIsRetriedAndRecovers) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    ASSERT_EQ(publisher.seq(), 1u);

    // Make the next write fail: the file's place is taken by a directory, which rename won't replace.
    std::filesystem::remove(file());
    std::filesystem::create_directories(file() / "blocker");
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_);
    ASSERT_TRUE(publisher.error());
    EXPECT_FALSE(publisher.published()) << "a directory holds no context";
    EXPECT_EQ(entries_in(file().parent_path()), 1) << "no temporary file left after the failed rename";

    std::filesystem::remove_all(file());
    publisher.update(pointing_at(2), PublishTrigger::background, t0_ + 1s);  // the context that failed, retried
    EXPECT_FALSE(publisher.error());
    EXPECT_EQ(publisher.seq(), 2u) << "one more successful write";
    EXPECT_EQ(read_file()["point"]["order"]["orderId"], 2);
}

TEST_F(Publisher, AFailedWriteIsRetriedWhenTheContextReturnsToWhatWasPublished) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    std::filesystem::remove(file());
    std::filesystem::create_directories(file() / "blocker");
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_);
    ASSERT_TRUE(publisher.error());

    // The user goes back to what was published. The context equals published(), but the last write
    // failed, so it is written again; otherwise the error would stay on screen for good.
    std::filesystem::remove_all(file());
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_ + 1s);
    EXPECT_FALSE(publisher.error());
    EXPECT_EQ(publisher.seq(), 2u);
    EXPECT_TRUE(std::filesystem::exists(file()));
}

// The file removed by someone else, then a write that fails: no file holds a context.
TEST_F(Publisher, AFailedWriteWithTheFileGoneClaimsNothing) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    std::filesystem::remove(file());
    std::filesystem::create_directories(std::format("{}.{}.tmp", file().string(), ::getpid()));
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_);
    ASSERT_TRUE(publisher.error());
    EXPECT_FALSE(publisher.published());
    EXPECT_FALSE(publisher.published_bar());
}

// A directory that can't be written: the write fails and the old file can't be removed, so it
// still holds what was last published, and published() says so. Root writes anywhere, so this
// can't be arranged as root (CI's container).
TEST_F(Publisher, AFailedWriteThatCantRemoveTheFileKeepsWhatItHolds) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can write to a read-only directory";
    }
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    const auto directory = file().parent_path();
    std::filesystem::permissions(directory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    ASSERT_TRUE(publisher.error());
    EXPECT_EQ(publisher.published(), pointing_at(1));
    EXPECT_EQ(read_file()["point"]["order"]["orderId"], 1);
}

TEST_F(Publisher, AFailedWriteRemovesTheOldFileWhenItCan) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    ASSERT_TRUE(publisher.published_bar());
    // The temporary file's place is taken by a directory, so the write fails, but the directory is
    // writable, so the old file can be removed.
    std::filesystem::create_directories(std::format("{}.{}.tmp", file().string(), ::getpid()));
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_);
    ASSERT_TRUE(publisher.error());
    EXPECT_FALSE(std::filesystem::exists(file())) << "no file, rather than one the user has since changed";
    EXPECT_FALSE(publisher.published()) << "and nothing claims it is published";
    EXPECT_FALSE(publisher.published_bar());
}

TEST_F(Publisher, TheBarTextIsThePublishedContexts) {
    ContextPublisher publisher{file()};
    EXPECT_FALSE(publisher.published_bar());
    publisher.update(pointing_at(3), PublishTrigger::user_action, t0_);
    EXPECT_EQ(publisher.published_bar(), bar_text(*publisher.published()));
}

TEST_F(Publisher, ABareFileNameIsWrittenInTheWorkingDirectory) {
    std::filesystem::create_directories(dir_);
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(dir_);
    const auto written = write_atomically("bare.json", "{}\n");
    std::filesystem::current_path(previous);
    ASSERT_TRUE(written.has_value()) << written.error();
    EXPECT_TRUE(std::filesystem::exists(dir_ / "bare.json"));
}

// ---- stop signals ----

TEST(StopSignals, SigtermAsksTheLoopToStopInsteadOfEndingTheProcess) {
    ASSERT_TRUE(install_stop_signals().has_value());
    EXPECT_FALSE(stop_requested());
    ASSERT_EQ(std::raise(SIGTERM), 0);  // without the handler, this would end the test program
    EXPECT_TRUE(stop_requested());
}

// A viewer stuck before it reads the flag can still be ended: the second signal gets the default
// action. Run in a child, which the second signal ends.
TEST(StopSignals, ASecondSignalEndsTheProcess) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        if (!install_stop_signals()) {
            ::_exit(2);
        }
        std::raise(SIGINT);
        if (!stop_requested()) {
            ::_exit(3);
        }
        std::raise(SIGINT);
        ::_exit(0);  // reached only if the second signal was caught too
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status)) << "the child exited with " << WEXITSTATUS(status);
    EXPECT_EQ(WTERMSIG(status), SIGINT);
}

}  // namespace
}  // namespace fillscope
