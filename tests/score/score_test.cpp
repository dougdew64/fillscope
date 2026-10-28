// The detector's check (score/score.h): which clients and trades are targets, and sets of flags made
// by hand scored against a session made by hand (support/session_fixture.h), with one trade more,
// that no target was in. That a real session's targets are what its recording says is checked by
// session_system_tests.
#include "score/score.h"
#include "support/session_fixture.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <vector>

namespace fillscope {
namespace {

// The hand-made session's updates, then on ticker 2:
//   14-16 the maker's buy, order 3, met by a random sell, order 4: the only trade no target was in
std::vector<MarketUpdate> updates() {
    std::vector<MarketUpdate> all = test::session_updates();
    all.push_back(test::session_update(14, UpdateType::add, 3, 2, Side::buy, 49, 2));
    all.push_back(test::session_update(15, UpdateType::trade, 0, 2, Side::sell, 49, 2));
    all.push_back(test::session_update(16, UpdateType::cancel, 3, 2, Side::buy, 49, 0));
    return all;
}

Truth truth() {
    const Truth hand = test::session_truth_of();
    std::vector<OrderTruth> orders = hand.orders();
    orders.push_back({2, 3, 1});
    orders.push_back({2, 4, 5});
    std::vector<TradeTruth> trades = hand.trades();
    trades.push_back({.update = 15, .incoming_client = 5, .incoming_order_id = 4, .resting_client = 1, .resting_order_id = 3});
    return Truth{hand.recording(), orders, trades};
}

// The slicer, 6, and the taker, 3.
const std::set<std::uint32_t> kTargets{3, 6};

Score scored(const std::set<std::uint64_t>& flagged) {
    const std::vector<MarketUpdate> all = updates();
    auto score = score_flags(all, truth(), kTargets, flagged);
    EXPECT_TRUE(score) << score.error();
    return score.value_or(Score{});
}

TEST(Targets, AreTheClientsThatTradeOneSideAndSizeRepeatedly) {
    EXPECT_TRUE(is_target_strategy("SLICER"));
    EXPECT_TRUE(is_target_strategy("TAKER"));
    EXPECT_FALSE(is_target_strategy("MAKER"));
    EXPECT_FALSE(is_target_strategy("RANDOM"));
    EXPECT_FALSE(is_target_strategy("slicer"));  // as session.json names it, exactly
}

TEST(Targets, AreReadFromTheSessionsClients) {
    const test::TemporaryDirectory directory{"fillscope-score"};
    test::write_session(directory.path);
    const auto targets = read_targets(directory.path / "session.json");
    ASSERT_TRUE(targets) << targets.error();
    EXPECT_EQ(*targets, kTargets);
}

TEST(Targets, CantBeReadFromAFileThatIsntASessions) {
    const test::TemporaryDirectory directory{"fillscope-score"};
    EXPECT_EQ(read_targets(directory.path / "none.json").error(), std::format("can't open {}", (directory.path / "none.json").string()));
    std::ofstream{directory.path / "session.json"} << R"({"clients": [{"client": 6}]})";
    const auto targets = read_targets(directory.path / "session.json");
    ASSERT_FALSE(targets);
    EXPECT_TRUE(targets.error().starts_with(std::format("{} isn't a session's: ", (directory.path / "session.json").string())))
        << targets.error();
}

// The slicer took (4), was met (7, and 12 on ticker 2), the taker took (9); the trade at 15 was
// the maker's and the random client's.
TEST(TargetTrades, AreThoseATargetWasOnEitherSideOf) {
    EXPECT_EQ(target_trades(truth(), kTargets), (std::set<std::uint64_t>{4, 7, 9, 12}));
    EXPECT_EQ(target_trades(truth(), {6}), (std::set<std::uint64_t>{4, 7, 12}));
    EXPECT_EQ(target_trades(truth(), {1}), (std::set<std::uint64_t>{4, 15}));  // the maker's order resting in each
    EXPECT_EQ(target_trades(truth(), {}), std::set<std::uint64_t>{});
}

TEST(Score, NothingFlaggedMissesEveryTarget) {
    EXPECT_EQ(scored({}), (Score{.all = {.trades = 5, .targets = 4, .hits = 0, .misses = 4, .false_alarms = 0},
                                  .by_ticker = {{1, {.trades = 3, .targets = 3, .hits = 0, .misses = 3, .false_alarms = 0}},
                                                {2, {.trades = 2, .targets = 1, .hits = 0, .misses = 1, .false_alarms = 0}}}}));
}

TEST(Score, EverythingFlaggedHitsEveryTargetAndFalselyAlarmsOnTheRest) {
    EXPECT_EQ(scored({4, 7, 9, 12, 15}),
              (Score{.all = {.trades = 5, .targets = 4, .hits = 4, .misses = 0, .false_alarms = 1},
                     .by_ticker = {{1, {.trades = 3, .targets = 3, .hits = 3, .misses = 0, .false_alarms = 0}},
                                   {2, {.trades = 2, .targets = 1, .hits = 1, .misses = 0, .false_alarms = 1}}}}));
}

TEST(Score, ExactlyTheTargetsFlaggedIsPerfect) {
    EXPECT_EQ(scored({4, 7, 9, 12}),
              (Score{.all = {.trades = 5, .targets = 4, .hits = 4, .misses = 0, .false_alarms = 0},
                     .by_ticker = {{1, {.trades = 3, .targets = 3, .hits = 3, .misses = 0, .false_alarms = 0}},
                                   {2, {.trades = 2, .targets = 1, .hits = 1, .misses = 0, .false_alarms = 0}}}}));
}

TEST(Score, SomeOfEachIsCountedOnEachTicker) {
    EXPECT_EQ(scored({4, 12, 15}),
              (Score{.all = {.trades = 5, .targets = 4, .hits = 2, .misses = 2, .false_alarms = 1},
                     .by_ticker = {{1, {.trades = 3, .targets = 3, .hits = 1, .misses = 2, .false_alarms = 0}},
                                   {2, {.trades = 2, .targets = 1, .hits = 1, .misses = 0, .false_alarms = 1}}}}));
}

TEST(Score, WithNoTargetsEveryFlagIsAFalseAlarm) {
    const std::vector<MarketUpdate> all = updates();
    const auto score = score_flags(all, truth(), {}, {4, 9});
    ASSERT_TRUE(score) << score.error();
    EXPECT_EQ(score->all, (Tally{.trades = 5, .targets = 0, .hits = 0, .misses = 0, .false_alarms = 2}));
}

TEST(Score, AFlagOnAnUpdateThatIsntATradeIsRefused) {
    const std::vector<MarketUpdate> all = updates();
    EXPECT_EQ(score_flags(all, truth(), kTargets, {4, 5}).error(), "update 5 is flagged, but isn't a trade");
    EXPECT_EQ(score_flags(all, truth(), kTargets, {0}).error(), "update 0 is flagged, but isn't a trade");
    EXPECT_EQ(score_flags(all, truth(), kTargets, {17}).error(), "update 17 is flagged, but isn't a trade");
}

TEST(Score, ATruthThatIsntTheMarketDatasIsRefused) {
    const std::vector<MarketUpdate> all = updates();
    // The hand-made truth, without the trade at 15.
    EXPECT_EQ(score_flags(all, test::session_truth_of(), kTargets, {}).error(),
              "the market data has 5 trades, its truth 4: not this market data's truth");
    // As many trades, but one at an update that isn't a trade.
    std::vector<TradeTruth> shifted = test::session_truth_of().trades();
    shifted.push_back({.update = 16, .incoming_client = 5, .incoming_order_id = 4, .resting_client = 1, .resting_order_id = 3});
    EXPECT_EQ(score_flags(all, Truth{"requests.rec", truth().orders(), shifted}, kTargets, {}).error(),
              "the truth has a trade at update 16, which the market data doesn't");
}

}  // namespace
}  // namespace fillscope
