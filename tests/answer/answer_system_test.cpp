// The answer tool run whole (src/answer/main.cpp): scenarios played through the back end's real
// exchange_replay, their claims checked against what its matcher really did. So these also pin down
// how Ghosh's exchange matches, as an answer will state it: a sweep across two prices, a rejected
// cancel, a fill at the resting order's price.
//
//   answer_system_tests FILLSCOPE_ANSWER
#include "support/test_support.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

namespace fillscope {
namespace {

std::string g_answer_tool;

using Json = nlohmann::json;

class AnswerTool : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               std::format("fillscope-answer-tool-{}-{}", ::getpid(), ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    // Writes `scenario` and runs the tool on it, its answer in this test's directory.
    test::CommandResult run(const Json& scenario) const {
        std::ofstream{dir_ / "scenario.json"} << scenario.dump(1);
        return test::run_command(std::format("{} {} --answer {}", test::shell_quote(g_answer_tool),
                                             test::shell_quote((dir_ / "scenario.json").string()),
                                             test::shell_quote(answer_file().string())));
    }

    [[nodiscard]] std::filesystem::path answer_file() const { return dir_ / ".fillscope" / "answer.json"; }

    [[nodiscard]] Json answer() const {
        std::ifstream in{answer_file()};
        return Json::parse(in);
    }

    std::filesystem::path dir_;
};

Json scenario(const Json& steps) {
    return Json{{"question", "How does matching work?"},
                {"clients", {{"1", "the maker"}, {"3", "the taker"}}},
                {"steps", steps}};
}

Json step(const Json& request, const Json& claims) {
    return Json{{"request", request}, {"say", "Something happens."}, {"claims", claims}};
}

Json order(int client, int id, const char* side, int price, int qty) {
    return Json{{"client", client}, {"new", {{"id", id}, {"side", side}, {"price", price}, {"qty", qty}}}};
}

TEST_F(AnswerTool, WritesTheAnswerWhenEveryClaimHolds) {
    const auto result = run(scenario(Json::array({
        step(order(1, 10, "buy", 99, 10), Json::parse(R"([{"bestBid": {"price": 99, "qty": 10}}, {"trades": []}])")),
        step(order(1, 11, "sell", 101, 10), Json::parse(R"([{"bestAsk": {"price": 101, "qty": 10}}])")),
        step(order(3, 30, "buy", 101, 4), Json::parse(R"([
            {"trades": [{"price": 101, "qty": 4, "aggressor": "buy"}]},
            {"told": {"client": 1, "filled": {"id": 11, "price": 101, "qty": 4, "leaves": 6}}},
            {"told": {"client": 3, "filled": {"id": 30, "price": 101, "qty": 4, "leaves": 0}}},
            {"bestAsk": {"price": 101, "qty": 6}},
            {"resting": {"client": 3, "id": 30, "qty": null}}])")),
    })));
    ASSERT_EQ(result.status, 0) << result.output;
    EXPECT_NE(result.output.find("3 steps, 8 claims, all true"), std::string::npos) << result.output;
    const Json written = answer();
    ASSERT_EQ(written["steps"].size(), 3u);
    const Json& crossing = written["steps"][2];
    EXPECT_EQ(crossing["request"], "The taker sends order 30: buy 4 at 101.");
    EXPECT_EQ(crossing["updates"]["first"], 3) << "steps 1 and 2 made one update each";
    EXPECT_EQ(crossing["updates"]["count"], 2) << "the trade, and the resting sell's new quantity";
    // The incoming order is accepted, then each side is told of the fill, the incoming one first.
    ASSERT_EQ(crossing["responses"].size(), 3u);
    EXPECT_EQ(crossing["responses"][0]["says"], "order 30 accepted: buy 4 at 101");
    EXPECT_EQ(crossing["responses"][1]["to"], "the taker");
    EXPECT_EQ(crossing["responses"][2]["to"], "the maker");
    EXPECT_TRUE(std::filesystem::exists(written["replay"]["marketDataFile"].get<std::string>()));
    EXPECT_TRUE(written["replay"]["marketDataFingerprint"].get<std::string>().starts_with("fnv1a64:"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / ".fillscope" / "answer" / "exchange_matching_engine.log"))
        << "the replay's own log goes in the work directory";
}

// A false claim is never shown: the tool says which, and what is true, and the answer file is left
// as it was.
TEST_F(AnswerTool, AFalseClaimIsRefusedAndTheAnswerLeftAsItWas) {
    ASSERT_EQ(run(scenario(Json::array({step(order(1, 10, "buy", 99, 10), Json::array())}))).status, 0);
    const Json before = answer();
    const auto result = run(scenario(Json::array({
        step(order(1, 10, "buy", 99, 10), Json::parse(R"([{"bestBid": {"price": 99, "qty": 12}}])")),
    })));
    EXPECT_EQ(result.status, 1) << result.output;
    EXPECT_NE(result.output.find("step 1: claims \"The best bid is 99, for 12.\", but the best bid is 99, for 10"),
              std::string::npos)
        << result.output;
    EXPECT_EQ(answer(), before);
}

// An incoming order takes the best price first, then the next, each at the resting order's price,
// and the rest of the book stays.
TEST_F(AnswerTool, ABuySweepsTheAsksBestFirst) {
    const auto result = run(scenario(Json::array({
        step(order(1, 10, "sell", 101, 5), Json::array()),
        step(order(1, 11, "sell", 102, 5), Json::array()),
        step(order(3, 30, "buy", 102, 8), Json::parse(R"([
            {"trades": [{"price": 101, "qty": 5, "aggressor": "buy"}, {"price": 102, "qty": 3, "aggressor": "buy"}]},
            {"told": {"client": 1, "filled": {"id": 10, "price": 101, "qty": 5, "leaves": 0}}},
            {"told": {"client": 1, "filled": {"id": 11, "price": 102, "qty": 3, "leaves": 2}}},
            {"resting": {"client": 1, "id": 10, "qty": null}},
            {"bestAsk": {"price": 102, "qty": 2}},
            {"bestBid": null}])")),
    })));
    EXPECT_EQ(result.status, 0) << result.output;
}

// What doesn't trade rests: there is no immediate-or-cancel order in Ghosh's exchange.
TEST_F(AnswerTool, WhatAnOrderCantTradeRests) {
    const auto result = run(scenario(Json::array({
        step(order(1, 10, "sell", 101, 3), Json::array()),
        step(order(3, 30, "buy", 101, 5), Json::parse(R"([
            {"trades": [{"price": 101, "qty": 3, "aggressor": "buy"}]},
            {"resting": {"client": 3, "id": 30, "qty": 2}},
            {"bestBid": {"price": 101, "qty": 2}},
            {"bestAsk": null}])")),
    })));
    EXPECT_EQ(result.status, 0) << result.output;
}

TEST_F(AnswerTool, ACancelTakesAnOrderOutAndACancelOfNothingIsRejected) {
    const auto result = run(scenario(Json::array({
        step(order(1, 10, "buy", 99, 10), Json::array()),
        step(Json::parse(R"({"client": 1, "cancel": {"id": 10}})"), Json::parse(R"([
            {"told": {"client": 1, "canceled": {"id": 10, "leaves": 10}}},
            {"resting": {"client": 1, "id": 10, "qty": null}},
            {"bestBid": null}])")),
        step(Json::parse(R"({"client": 1, "cancel": {"id": 10}})"), Json::parse(R"([
            {"told": {"client": 1, "cancelRejected": {"id": 10}}},
            {"trades": []}])")),
    })));
    ASSERT_EQ(result.status, 0) << result.output;
    EXPECT_EQ(answer()["steps"][2]["updates"]["count"], 0) << "a rejected cancel changes no book";
}

TEST_F(AnswerTool, RefusesWhatTheExchangeCantDoAndABadCommandLine) {
    const auto market = run(scenario(Json::array({
        step(Json::parse(R"({"client": 3, "market": {"id": 30, "side": "buy", "qty": 4}})"), Json::array())})));
    EXPECT_EQ(market.status, 1);
    EXPECT_NE(market.output.find("no market orders"), std::string::npos) << market.output;
    EXPECT_FALSE(std::filesystem::exists(answer_file()));
    EXPECT_EQ(test::run_command(test::shell_quote(g_answer_tool)).status, 2);
    EXPECT_EQ(test::run_command(test::shell_quote(g_answer_tool) + " a.json --answer").status, 2);
    EXPECT_EQ(test::run_command(test::shell_quote(g_answer_tool) + " a.json b.json").status, 2);
    EXPECT_EQ(test::run_command(test::shell_quote(g_answer_tool) + " no-such-scenario.json").status, 1);
}

}  // namespace
}  // namespace fillscope

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: answer_system_tests FILLSCOPE_ANSWER\n");
        return 2;
    }
    fillscope::g_answer_tool = argv[1];
    return RUN_ALL_TESTS();
}
