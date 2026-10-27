// A recorded session's truth as the viewer reads and shows it (viewer/session_truth.h): read from its
// three files, refused when its truth isn't its market data's, and each trade marked from it.
#include "support/session_fixture.h"
#include "support/test_support.h"
#include "viewer/session_truth.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace fillscope {
namespace {

class SessionTruthTest : public ::testing::Test {
protected:
    const test::TemporaryDirectory dir_{"fillscope-session"};
};

TEST_F(SessionTruthTest, ReadsASessionsMarketDataTruthAndClients) {
    test::write_session(dir_.path);
    const auto session = read_session(dir_.path);
    ASSERT_TRUE(session) << session.error();
    EXPECT_EQ(session->market_data.updates, test::session_updates());
    EXPECT_EQ(session->truth.truth.trades(), test::session_truth_of().trades());
    EXPECT_EQ(session->truth.directory, std::filesystem::absolute(dir_.path));
    const SessionTruth expected = test::hand_session().truth;
    EXPECT_EQ(session->truth.roles, expected.roles);
    EXPECT_EQ(session->truth.slicers, expected.slicers);
    EXPECT_EQ(session->truth.slices, expected.slices);
}

TEST_F(SessionTruthTest, ATruthThatIsntItsMarketDatasIsRefused) {
    const auto error = [&](const Truth& truth) {
        test::write_session(dir_.path, test::session_json(), truth);
        const auto session = read_session(dir_.path);
        return session ? std::string{"(read)"} : session.error();
    };
    const Truth right = test::session_truth_of();
    EXPECT_NE(error(Truth{"other.rec", right.orders(), right.trades()}).find("it was replayed from other.rec, the market data from requests.rec"),
              std::string::npos);
    std::vector<TradeTruth> trades = right.trades();
    trades.erase(trades.begin() + 1);
    EXPECT_NE(error(Truth{"requests.rec", right.orders(), trades}).find("it has no trade at update 7, which is one"), std::string::npos);
    trades = right.trades();
    trades.push_back({.update = 13, .incoming_client = 5, .incoming_order_id = 9, .resting_client = 6, .resting_order_id = 2});
    EXPECT_NE(error(Truth{"requests.rec", right.orders(), trades}).find("it has 5 trades, the market data 4"), std::string::npos);
}

TEST_F(SessionTruthTest, ASessionMissingAFileOrWithABadOneSaysWhy) {
    EXPECT_NE(read_session(dir_.path / "none").error().find("can't open"), std::string::npos);
    test::write_session(dir_.path);
    std::filesystem::remove(dir_.path / "session.json");
    EXPECT_NE(read_session(dir_.path).error().find("can't open " + (dir_.path / "session.json").string()), std::string::npos);
    std::ofstream{dir_.path / "session.json"} << R"({"clients": [{"client": 6}]})";
    EXPECT_NE(read_session(dir_.path).error().find("isn't a session's"), std::string::npos);
    std::filesystem::remove(dir_.path / "truth.bin");
    EXPECT_NE(read_session(dir_.path).error().find("can't open " + (dir_.path / "truth.bin").string()), std::string::npos);
}

// Each trade on the ticker, in the window asked for, marked by whether the slicer's order came in
// and took, or rested and was met, as the truth says.
TEST_F(SessionTruthTest, EachTradeIsMarkedFromTheTruth) {
    const Session session = test::hand_session();
    const std::vector<MarketUpdate>& updates = session.market_data.updates;
    using S = TradeDot::Slicer;
    const std::vector<TradeDot> all{{.update = 4, .price = 105, .qty = 10, .side = Side::buy, .slicer = S::took},
                                    {.update = 7, .price = 105, .qty = 5, .side = Side::sell, .slicer = S::met},
                                    {.update = 9, .price = 106, .qty = 4, .side = Side::buy, .slicer = S::none}};
    EXPECT_EQ(trade_dots(updates, session.truth, 1, 1, 13), all);
    EXPECT_EQ(trade_dots(updates, session.truth, 1, 0, 100), all) << "a window past the ends is cut to them";
    EXPECT_EQ(trade_dots(updates, session.truth, 1, 5, 8), std::vector<TradeDot>{all[1]});
    EXPECT_EQ(trade_dots(updates, session.truth, 1, 1, 3), std::vector<TradeDot>{});
    EXPECT_EQ(trade_dots(updates, session.truth, 2, 1, 13),
              (std::vector<TradeDot>{{.update = 12, .price = 50, .qty = 3, .side = Side::buy, .slicer = S::took}}));
    // With no slicer named, no trade is the slicer's.
    SessionTruth none = session.truth;
    none.slicers.clear();
    for (const TradeDot& dot : trade_dots(updates, none, 1, 1, 13)) {
        EXPECT_EQ(dot.slicer, S::none) << dot.update;
    }
}

// The nearest trade on a ticker either way, and the nearest of the slicer's: on ticker 1, trades at
// 4 (the slicer took), 7 (its resting order was met) and 9 (not the slicer's); ticker 2's at 12.
TEST_F(SessionTruthTest, TheNearestTradeEitherWayIsFound) {
    const Session session = test::hand_session();
    const std::vector<MarketUpdate>& u = session.market_data.updates;
    using W = Way;
    EXPECT_EQ(nearest_trade(u, 1, 0, W::forward), 4u);
    EXPECT_EQ(nearest_trade(u, 1, 4, W::forward), 7u) << "after, not at";
    EXPECT_EQ(nearest_trade(u, 1, 7, W::forward), 9u);
    EXPECT_EQ(nearest_trade(u, 1, 9, W::forward), std::nullopt);
    EXPECT_EQ(nearest_trade(u, 1, 13, W::back), 9u);
    EXPECT_EQ(nearest_trade(u, 1, 9, W::back), 7u) << "before, not at";
    EXPECT_EQ(nearest_trade(u, 1, 4, W::back), std::nullopt);
    EXPECT_EQ(nearest_trade(u, 1, 100, W::back), 9u) << "from past the end";
    EXPECT_EQ(nearest_trade(u, 2, 0, W::forward), 12u);
    const SessionTruth* slicer = &session.truth;
    EXPECT_EQ(nearest_trade(u, 1, 0, W::forward, slicer), 4u);
    EXPECT_EQ(nearest_trade(u, 1, 4, W::forward, slicer), 7u) << "its resting order met counts";
    EXPECT_EQ(nearest_trade(u, 1, 7, W::forward, slicer), std::nullopt) << "9 isn't the slicer's";
    EXPECT_EQ(nearest_trade(u, 1, 13, W::back, slicer), 7u);
}

TEST_F(SessionTruthTest, AClientIsNamedWithItsRole) {
    const SessionTruth truth = test::hand_session().truth;
    EXPECT_EQ(client_name(truth, 6), "6 (slicer)");
    EXPECT_EQ(client_name(truth, 1), "1 (maker)");
    EXPECT_EQ(client_name(truth, 9), "9");
}

}  // namespace
}  // namespace fillscope
