// The answer tool's parts (src/answer/): reading a scenario, the bytes of the back end's recording
// and steps files, and checking claims. The tool run whole, through the real exchange_replay, is in
// answer_system_test.cpp.
#include "answer/answer.h"
#include "answer/check.h"
#include "answer/scenario.h"
#include "answer/wire.h"
#include "feed/book.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fillscope {
namespace {

using Json = nlohmann::json;

// The example of docs/ARCHITECTURE.md's *Answers*: a maker quotes both sides, a taker crosses.
Json maker_and_taker() {
    return Json::parse(R"({
      "question": "What happens when a taker trades with a maker?",
      "ticker": 2,
      "clients": { "1": "the maker", "3": "the taker" },
      "intro": "A maker posts orders that wait; a taker trades with them at once.",
      "steps": [
        { "request": { "client": 1, "new": { "id": 10, "side": "buy", "price": 99, "qty": 10 } },
          "say": "The maker bids.",
          "claims": [ { "bestBid": { "price": 99, "qty": 10 } }, { "trades": [] } ] },
        { "request": { "client": 1, "new": { "id": 11, "side": "sell", "price": 101, "qty": 10 } },
          "say": "And offers." },
        { "request": { "client": 3, "new": { "id": 30, "side": "buy", "price": 101, "qty": 4 } },
          "say": "The taker crosses.",
          "claims": [ { "trades": [ { "price": 101, "qty": 4, "aggressor": "buy" } ] },
                      { "told": { "client": 1, "filled": { "id": 11, "price": 101, "qty": 4, "leaves": 6 } } },
                      { "resting": { "client": 3, "id": 30, "qty": null } },
                      { "bestAsk": null } ] },
        { "request": { "client": 1, "cancel": { "id": 10 } }, "say": "The maker withdraws its bid." }
      ]
    })");
}

// The reason `json` is refused, or "" if it isn't.
std::string refusal(const Json& json) {
    const auto read = read_scenario(json);
    return read ? std::string{} : read.error();
}

// maker_and_taker() with one step's request replaced.
Json with_request(std::size_t step, const Json& request) {
    Json json = maker_and_taker();
    json["steps"][step]["request"] = request;
    return json;
}

Json with_claim(const Json& claim) {
    Json json = maker_and_taker();
    json["steps"][0]["claims"] = Json::array({claim});
    return json;
}

// ---- Reading a scenario ----

TEST(ReadScenario, ReadsEveryPart) {
    const auto read = read_scenario(maker_and_taker());
    ASSERT_TRUE(read) << read.error();
    EXPECT_EQ(read->question, "What happens when a taker trades with a maker?");
    EXPECT_EQ(read->ticker, 2u);
    EXPECT_EQ(read->clients.at(1), "the maker");
    EXPECT_EQ(read->clients.at(3), "the taker");
    ASSERT_EQ(read->steps.size(), 4u);
    EXPECT_EQ(read->steps[0].request,
              (Request{.client = 1, .action = NewOrder{.id = 10, .side = Side::buy, .price = 99, .qty = 10}}));
    EXPECT_EQ(read->steps[3].request, (Request{.client = 1, .action = Cancel{.id = 10}}));
    EXPECT_TRUE(read->steps[1].claims.empty()) << "claims are optional";
    ASSERT_EQ(read->steps[2].claims.size(), 4u);
    EXPECT_EQ(read->steps[2].claims[0],
              (Claim{TradesClaim{.trades = {{.price = 101, .qty = 4, .aggressor = Side::buy}}}}));
    EXPECT_EQ(read->steps[2].claims[1], (Claim{ToldClaim{.client = 1, .kind = ResponseKind::filled, .id = 11,
                                                         .price = 101, .qty = 4, .leaves = 6}}));
    EXPECT_EQ(read->steps[2].claims[2], (Claim{RestingClaim{.client = 3, .id = 30, .qty = std::nullopt}}));
    EXPECT_EQ(read->steps[2].claims[3], (Claim{BestClaim{.side = Side::sell, .best = std::nullopt}}));
}

// What Ghosh's exchange can't be asked to do is refused in the domain's words, not played as
// something else.
TEST(ReadScenario, RefusesWhatTheExchangeCantDoSayingWhy) {
    EXPECT_NE(refusal(with_request(2, Json::parse(R"({"client": 3, "market": {"id": 30, "side": "buy", "qty": 4}})")))
                  .find("no market orders"),
              std::string::npos);
    EXPECT_NE(refusal(with_request(2, Json::parse(R"({"client": 1, "modify": {"id": 11, "qty": 4}})")))
                  .find("can't modify an order"),
              std::string::npos);
    EXPECT_NE(refusal(with_request(2, Json::parse(R"({"client": 1, "new": {"id": 12, "side": "buy", "price": 99, "qty": 1},
                                                      "cancel": {"id": 10}})")))
                  .find("exactly one of 'new' and 'cancel'"),
              std::string::npos);
}

TEST(ReadScenario, RefusesAnOrderTheMatcherCouldntTake) {
    const auto order = [](const char* fields) {
        return with_request(2, Json::parse(std::string{R"({"client": 3, "new": {)"} + fields + "}}"));
    };
    const std::vector<std::pair<const char*, const char*>> cases = {
        {R"("id": 30, "side": "buy", "price": 101.5, "qty": 4)", "must be a whole number"},
        {R"("id": 30, "side": "buy", "price": 0, "qty": 4)", "must be from 1"},
        {R"("id": 30, "side": "buy", "price": 101, "qty": 0)", "must be from 1"},
        {R"("id": 30, "side": "long", "price": 101, "qty": 4)", "must be \"buy\" or \"sell\""},
        {R"("id": 1048576, "side": "buy", "price": 101, "qty": 4)", "step 3, request.new.id: is 1048576"},
        {R"("id": 30, "side": "buy", "price": 101)", "needs 'qty'"},
        {R"("id": 30, "side": "buy", "price": 101, "qty": 4, "tif": "ioc")", "has a field 'tif'"},
        {R"("id": 30, "side": "buy", "price": "101", "qty": 4)", "must be a whole number"},
        {R"("id": 30, "side": "buy", "price": 18446744073709551615, "qty": 4)", "may be at most"},
    };
    for (const auto& [fields, why] : cases) {
        EXPECT_NE(refusal(order(fields)).find(why), std::string::npos) << fields << ": " << refusal(order(fields));
    }
}

// The exchange keys a client's orders by its id, so a second new order under one would overwrite
// the first's entry: refused. Another client may use the same number.
TEST(ReadScenario, EachNewOrderNeedsAnIdOfItsOwn) {
    EXPECT_NE(refusal(with_request(1, Json::parse(R"({"client": 1, "new": {"id": 10, "side": "sell", "price": 101, "qty": 1}})")))
                  .find("uses order id 10 again for client 1"),
              std::string::npos);
    EXPECT_EQ(refusal(with_request(2, Json::parse(R"({"client": 3, "new": {"id": 10, "side": "buy", "price": 101, "qty": 4}})"))),
              "");
}

TEST(ReadScenario, RefusesAMisspeltOrMissingPart) {
    Json json = maker_and_taker();
    json["steps"][0]["sayy"] = "x";
    EXPECT_NE(refusal(json).find("step 1: has a field 'sayy'"), std::string::npos) << refusal(json);
    json = maker_and_taker();
    json["steps"][0].erase("say");
    EXPECT_NE(refusal(json).find("step 1: needs 'say'"), std::string::npos);
    json = maker_and_taker();
    json.erase("question");
    EXPECT_NE(refusal(json).find("needs 'question'"), std::string::npos);
    json = maker_and_taker();
    json["steps"] = Json::array();
    EXPECT_NE(refusal(json).find("at least one"), std::string::npos);
    json = maker_and_taker();
    json["ticker"] = 8;
    EXPECT_NE(refusal(json).find("must be from 0 to 7"), std::string::npos);
    json = maker_and_taker();
    json["clients"]["x"] = "someone";
    EXPECT_NE(refusal(json).find("'x' isn't a client number"), std::string::npos);
    EXPECT_FALSE(read_scenario(std::string_view{"{ not json"}));
}

TEST(ReadScenario, AClientMustHaveARole) {
    EXPECT_NE(refusal(with_request(0, Json::parse(R"({"client": 7, "new": {"id": 1, "side": "buy", "price": 99, "qty": 1}})")))
                  .find("names client 7, which 'clients' gives no role"),
              std::string::npos);
    EXPECT_NE(refusal(with_claim(Json::parse(R"({"resting": {"client": 7, "id": 1, "qty": 1}})"))).find("names client 7"),
              std::string::npos);
}

TEST(ReadScenario, RefusesAClaimThatIsntOne) {
    const std::vector<std::pair<const char*, const char*>> cases = {
        {R"({"bestbid": {"price": 99, "qty": 10}})", "'bestbid' is no kind of claim"},
        {R"({"bestBid": {"price": 99, "qty": 10}, "trades": []})", "must be an object with one field"},
        {R"({"bestBid": {"price": 99}})", "needs 'qty'"},
        {R"({"trades": {"price": 99}})", "must be a list of trades"},
        {R"({"trades": [{"price": 99, "qty": 1}]})", "needs 'aggressor'"},
        {R"({"resting": {"client": 1, "id": 10}})", "needs 'qty': the quantity it rests with, or null"},
        {R"({"told": {"client": 1, "accepted": {"id": 10}, "filled": {"id": 10}}})", "more than one response"},
        {R"({"told": {"client": 1}})", "needs one of accepted, filled, canceled and cancelRejected"},
        {R"({"told": {"client": 1, "filled": {"id": 10, "leftover": 1}}})", "has a field 'leftover'"},
    };
    for (const auto& [claim, why] : cases) {
        const std::string said = refusal(with_claim(Json::parse(claim)));
        EXPECT_NE(said.find(why), std::string::npos) << claim << ": " << said;
    }
}

// ---- The back end's files, as bytes ----

// A field's bytes from `at`, little-endian.
template <typename T>
T field(const std::byte* bytes, std::size_t at) {
    T value;
    std::memcpy(&value, bytes + at, sizeof value);
    return value;
}

// The layout of MEClientRequest, field by field (backend/Chapter12/exchange/order_server/client_request.h).
TEST(Wire, ARequestIsTheBackEndsThirtyBytes) {
    const RequestBytes order = encode_request({.client = 3, .action = NewOrder{.id = 30, .side = Side::sell, .price = -5, .qty = 4}}, 2);
    EXPECT_EQ(field<std::uint8_t>(order.data(), 0), 1u) << "NEW";
    EXPECT_EQ(field<std::uint32_t>(order.data(), 1), 3u);
    EXPECT_EQ(field<std::uint32_t>(order.data(), 5), 2u);
    EXPECT_EQ(field<std::uint64_t>(order.data(), 9), 30u);
    EXPECT_EQ(field<std::int8_t>(order.data(), 17), -1) << "SELL";
    EXPECT_EQ(field<std::int64_t>(order.data(), 18), -5);
    EXPECT_EQ(field<std::uint32_t>(order.data(), 26), 4u);
    const RequestBytes cancel = encode_request({.client = 1, .action = Cancel{.id = 10}}, 0);
    EXPECT_EQ(field<std::uint8_t>(cancel.data(), 0), 2u) << "CANCEL";
    EXPECT_EQ(field<std::uint32_t>(cancel.data(), 1), 1u);
    EXPECT_EQ(field<std::uint64_t>(cancel.data(), 9), 10u);
    EXPECT_EQ(field<std::int8_t>(cancel.data(), 17), 0) << "no side";
}

class WireFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               std::format("fillscope-answer-{}-{}", ::getpid(), ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    void write(const std::filesystem::path& path, const std::string& bytes) const {
        std::ofstream{path, std::ios::binary} << bytes;
    }
    [[nodiscard]] std::string read(const std::filesystem::path& path) const {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    std::filesystem::path dir_;
};

TEST_F(WireFiles, ARecordingIsItsHeaderThenEachRequest) {
    const std::vector<Request> requests = {{.client = 1, .action = NewOrder{.id = 10, .side = Side::buy, .price = 99, .qty = 10}},
                                           {.client = 1, .action = Cancel{.id = 10}}};
    ASSERT_TRUE(write_recording(dir_ / "a.rec", requests, 2));
    const std::string bytes = read(dir_ / "a.rec");
    ASSERT_EQ(bytes.size(), 8 + 2 * kRequestSize);
    EXPECT_EQ(bytes.substr(0, 8), "LLREQ001");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const RequestBytes expected = encode_request(requests[i], 2);
        EXPECT_EQ(std::memcmp(bytes.data() + 8 + i * kRequestSize, expected.data(), kRequestSize), 0) << i;
    }
    EXPECT_FALSE(write_recording(dir_ / "no-such-directory" / "a.rec", requests, 2));
}

// The layout of the steps file and of MEClientResponse (the back end's matcher/replay.h and
// order_server/client_response.h): a file built field by field here reads back as written.
TEST_F(WireFiles, TheStepsFileReadsAsTheReplayWritesIt) {
    std::string bytes = "LLSTEP01";
    const auto append = [&bytes](auto value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof value); };
    append(std::uint32_t{5});
    bytes += "a.rec";
    const RequestBytes request = encode_request({.client = 3, .action = NewOrder{.id = 30, .side = Side::buy, .price = 101, .qty = 4}}, 0);
    bytes.append(reinterpret_cast<const char*>(request.data()), request.size());
    append(std::uint64_t{3});
    append(std::uint32_t{2});
    append(std::uint32_t{1});
    // One response: filled, to client 1, ticker 0, its order 11, the exchange's 2, sell, at 101,
    // 4 executed, 6 left.
    append(std::uint8_t{3});
    append(std::uint32_t{1});
    append(std::uint32_t{0});
    append(std::uint64_t{11});
    append(std::uint64_t{2});
    append(std::int8_t{-1});
    append(std::int64_t{101});
    append(std::uint32_t{4});
    append(std::uint32_t{6});
    write(dir_ / "s.dat", bytes);

    const auto file = read_steps_file(dir_ / "s.dat");
    ASSERT_TRUE(file) << file.error();
    EXPECT_EQ(file->recording, "a.rec");
    ASSERT_EQ(file->steps.size(), 1u);
    EXPECT_EQ(file->steps[0].request, request);
    EXPECT_EQ(file->steps[0].first_update, 3u);
    EXPECT_EQ(file->steps[0].update_count, 2u);
    ASSERT_EQ(file->steps[0].responses.size(), 1u);
    EXPECT_EQ(file->steps[0].responses[0],
              (ClientResponse{.type = ResponseType::filled, .client = 1, .ticker = 0, .client_order_id = 11,
                              .market_order_id = 2, .side = Side::sell, .price = 101, .exec_qty = 4, .leaves_qty = 6}));
    const std::vector<std::byte> encoded = encode_steps_file(*file);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()), bytes) << "and writes back the same";
}

TEST_F(WireFiles, AStepsFileThatIsntWholeIsRefused) {
    const StepsFile whole{.recording = "a.rec",
                          .steps = {{.request = {}, .first_update = 1, .update_count = 1,
                                     .responses = {{.type = ResponseType::accepted}}}}};
    const std::vector<std::byte> encoded = encode_steps_file(whole);
    const std::string bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    for (const std::size_t cut : {std::size_t{10}, std::size_t{15}, std::size_t{30}, bytes.size() - 1}) {
        write(dir_ / "s.dat", bytes.substr(0, cut));
        const auto read = read_steps_file(dir_ / "s.dat");
        ASSERT_FALSE(read) << "cut at " << cut;
        EXPECT_NE(read.error().find("cut short"), std::string::npos) << read.error();
    }
    std::string unknown = bytes;
    unknown[bytes.size() - kResponseSize] = static_cast<char>(9);
    write(dir_ / "s.dat", unknown);
    EXPECT_NE(read_steps_file(dir_ / "s.dat").error().find("type 9, which isn't one"), std::string::npos);
    write(dir_ / "s.dat", "LLMDAT01");
    EXPECT_NE(read_steps_file(dir_ / "s.dat").error().find("isn't a replay's steps"), std::string::npos);
    EXPECT_FALSE(read_steps_file(dir_ / "missing.dat"));
}

// ---- Checking claims ----

class Claims : public ::testing::Test {
protected:
    void SetUp() override {
        auto read = read_scenario(maker_and_taker());
        ASSERT_TRUE(read) << read.error();
        scenario_ = *read;
        // The book after step 3: the maker's bid 10 at 99 rests (the exchange's order 1); its ask
        // filled, 6 left (2).
        ASSERT_TRUE(book_.apply({.seq = 1, .type = UpdateType::add, .order_id = 1, .ticker = 2, .side = Side::buy, .price = 99, .qty = 10, .priority = 1}));
        ASSERT_TRUE(book_.apply({.seq = 2, .type = UpdateType::add, .order_id = 2, .ticker = 2, .side = Side::sell, .price = 101, .qty = 6, .priority = 1}));
        ids_ = {{{1, 10}, 1}, {{1, 11}, 2}, {{3, 30}, 3}};
    }

    [[nodiscard]] ClaimResult check(const Claim& claim) const {
        const StepState state{.book = book_, .updates = updates_, .responses = responses_, .market_ids = ids_};
        return check_claim(claim, state, scenario_);
    }

    Scenario scenario_;
    Book book_;
    MarketIds ids_;
    std::vector<MarketUpdate> updates_ = {
        {.seq = 3, .type = UpdateType::trade, .order_id = 0, .ticker = 2, .side = Side::buy, .price = 101, .qty = 4, .priority = 0},
        {.seq = 4, .type = UpdateType::modify, .order_id = 2, .ticker = 2, .side = Side::sell, .price = 101, .qty = 6, .priority = 1}};
    std::vector<ClientResponse> responses_ = {
        {.type = ResponseType::accepted, .client = 3, .ticker = 2, .client_order_id = 30, .market_order_id = 3, .side = Side::buy, .price = 101, .exec_qty = 0, .leaves_qty = 4},
        {.type = ResponseType::filled, .client = 3, .ticker = 2, .client_order_id = 30, .market_order_id = 3, .side = Side::buy, .price = 101, .exec_qty = 4, .leaves_qty = 0},
        {.type = ResponseType::filled, .client = 1, .ticker = 2, .client_order_id = 11, .market_order_id = 2, .side = Side::sell, .price = 101, .exec_qty = 4, .leaves_qty = 6}};
};

TEST_F(Claims, TheBestPriceOnASide) {
    const auto bid = check(BestClaim{.side = Side::buy, .best = PriceQty{.price = 99, .qty = 10}});
    EXPECT_TRUE(bid.holds);
    EXPECT_EQ(bid.says, "The best bid is 99, for 10.");
    const auto wrong = check(BestClaim{.side = Side::sell, .best = PriceQty{.price = 101, .qty = 10}});
    EXPECT_FALSE(wrong.holds);
    EXPECT_EQ(wrong.actually, "the best ask is 101, for 6");
    const auto empty = check(BestClaim{.side = Side::sell, .best = std::nullopt});
    EXPECT_FALSE(empty.holds) << "the sell side isn't empty";
    EXPECT_EQ(empty.says, "The sell side is empty.");
}

// Exactly the step's trades: one left out, one made up, or one of the wrong side is false.
TEST_F(Claims, TheStepsTradesExactly) {
    const auto right = check(TradesClaim{.trades = {{.price = 101, .qty = 4, .aggressor = Side::buy}}});
    EXPECT_TRUE(right.holds) << right.actually;
    EXPECT_EQ(right.says, "This step trades 4 at 101 (an incoming buy crossing).");
    const auto none = check(TradesClaim{.trades = {}});
    EXPECT_FALSE(none.holds);
    EXPECT_EQ(none.says, "This step makes no trade.");
    EXPECT_EQ(none.actually, "it makes 4 at 101 (an incoming buy crossing)");
    EXPECT_FALSE(check(TradesClaim{.trades = {{.price = 101, .qty = 4, .aggressor = Side::sell}}}).holds);
    EXPECT_FALSE(check(TradesClaim{.trades = {{.price = 101, .qty = 4, .aggressor = Side::buy},
                                              {.price = 101, .qty = 1, .aggressor = Side::buy}}})
                     .holds);
}

// A trade's side is the incoming order's: a sell crossing the bids is a sell, whichever side the
// resting order was.
TEST_F(Claims, TheAggressorIsTheIncomingOrdersSide) {
    updates_ = {{.seq = 3, .type = UpdateType::trade, .order_id = 0, .ticker = 2, .side = Side::sell, .price = 99, .qty = 2, .priority = 0}};
    EXPECT_TRUE(check(TradesClaim{.trades = {{.price = 99, .qty = 2, .aggressor = Side::sell}}}).holds);
    const auto buyer = check(TradesClaim{.trades = {{.price = 99, .qty = 2, .aggressor = Side::buy}}});
    EXPECT_FALSE(buyer.holds);
    EXPECT_EQ(buyer.actually, "it makes 2 at 99 (an incoming sell crossing)");
}

TEST_F(Claims, AnOrderRestingOrNot) {
    const auto rests = check(RestingClaim{.client = 1, .id = 11, .qty = 6});
    EXPECT_TRUE(rests.holds);
    EXPECT_EQ(rests.says, "The maker's order 11 rests with 6.");
    const auto wrong = check(RestingClaim{.client = 1, .id = 11, .qty = 10});
    EXPECT_FALSE(wrong.holds);
    EXPECT_EQ(wrong.actually, "it rests with 6");
    const auto gone = check(RestingClaim{.client = 3, .id = 30, .qty = std::nullopt});
    EXPECT_TRUE(gone.holds) << "filled whole, so it never rested";
    EXPECT_EQ(gone.says, "The taker's order 30 doesn't rest in the book.");
    const auto never = check(RestingClaim{.client = 3, .id = 31, .qty = 1});
    EXPECT_FALSE(never.holds);
    EXPECT_EQ(never.actually, "the exchange never accepted that order");
    EXPECT_FALSE(check(RestingClaim{.client = 1, .id = 10, .qty = std::nullopt}).holds) << "order 10 still rests";
}

// A response the client was sent in the step; the numbers given must all match one response.
TEST_F(Claims, WhatAClientWasTold) {
    const auto filled = check(ToldClaim{.client = 1, .kind = ResponseKind::filled, .id = 11, .price = 101, .qty = 4, .leaves = 6});
    EXPECT_TRUE(filled.holds) << filled.actually;
    EXPECT_EQ(filled.says, "The maker is told order 11 filled 4 at 101, with 6 left.");
    EXPECT_TRUE(check(ToldClaim{.client = 3, .kind = ResponseKind::accepted, .id = 30, .price = {}, .qty = {}, .leaves = {}}).holds);
    const auto leaves = check(ToldClaim{.client = 1, .kind = ResponseKind::filled, .id = 11, .price = {}, .qty = {}, .leaves = 5});
    EXPECT_FALSE(leaves.holds);
    EXPECT_EQ(leaves.actually, "it was told: order 11 filled 4 at 101, 6 left");
    EXPECT_FALSE(check(ToldClaim{.client = 3, .kind = ResponseKind::filled, .id = 11, .price = {}, .qty = {}, .leaves = {}}).holds)
        << "the fill of order 11 went to client 1";
    const auto nothing = check(ToldClaim{.client = 1, .kind = ResponseKind::canceled, .id = 10, .price = {}, .qty = {}, .leaves = {}});
    EXPECT_FALSE(nothing.holds);
    EXPECT_EQ(nothing.actually, "it was told nothing about order 10 in this step");
}

TEST(Describe, RequestsAndResponsesInWords) {
    const auto scenario = read_scenario(maker_and_taker());
    ASSERT_TRUE(scenario);
    EXPECT_EQ(describe(scenario->steps[0].request, *scenario), "The maker sends order 10: buy 10 at 99.");
    EXPECT_EQ(describe(scenario->steps[3].request, *scenario), "The maker cancels its order 10.");
    EXPECT_EQ(describe(ClientResponse{.type = ResponseType::canceled, .client = 1, .ticker = 2, .client_order_id = 10,
                                      .market_order_id = 1, .side = Side::buy, .price = 99, .exec_qty = kNoQty, .leaves_qty = 10}),
              "order 10 canceled, 10 unfilled");
    EXPECT_EQ(role_of(*scenario, 9), "client 9");
}

// ---- A scenario against its replay ----

// A replay of the first two steps of maker_and_taker(), as exchange_replay gives it.
struct TwoSteps {
    Scenario scenario;
    std::vector<MarketUpdate> updates;
    StepsFile steps;
};

TwoSteps two_steps() {
    Json json = maker_and_taker();
    json["steps"].erase(json["steps"].begin() + 2, json["steps"].end());
    TwoSteps t{.scenario = *read_scenario(json), .updates = {}, .steps = {}};
    t.updates = {{.seq = 1, .type = UpdateType::add, .order_id = 1, .ticker = 2, .side = Side::buy, .price = 99, .qty = 10, .priority = 1},
                 {.seq = 2, .type = UpdateType::add, .order_id = 2, .ticker = 2, .side = Side::sell, .price = 101, .qty = 10, .priority = 1}};
    for (std::size_t i = 0; i < 2; ++i) {
        t.steps.steps.push_back({.request = encode_request(t.scenario.steps[i].request, 2), .first_update = i + 1,
                                 .update_count = 1, .responses = {}});
    }
    t.steps.steps[0].responses = {{.type = ResponseType::accepted, .client = 1, .ticker = 2, .client_order_id = 10,
                                   .market_order_id = 1, .side = Side::buy, .price = 99, .exec_qty = 0, .leaves_qty = 10}};
    return t;
}

TEST(CheckScenario, ChecksEachStepAgainstTheBooksAfterIt) {
    const TwoSteps t = two_steps();
    const auto checked = check_scenario(t.scenario, t.updates, t.steps);
    ASSERT_TRUE(checked) << checked.error();
    ASSERT_EQ(checked->size(), 2u);
    ASSERT_EQ((*checked)[0].claims.size(), 2u);
    EXPECT_TRUE((*checked)[0].claims[0].holds) << "after step 1, the bid is 99 for 10, though step 2 follows";
    EXPECT_TRUE((*checked)[0].claims[1].holds);
    EXPECT_EQ(false_claims(*checked), "");
}

TEST(CheckScenario, AFalseClaimIsReportedWithWhatIsTrue) {
    TwoSteps t = two_steps();
    t.scenario.steps[0].claims[0] = BestClaim{.side = Side::buy, .best = PriceQty{.price = 98, .qty = 10}};
    const auto checked = check_scenario(t.scenario, t.updates, t.steps);
    ASSERT_TRUE(checked);
    EXPECT_EQ(false_claims(*checked), "step 1: claims \"The best bid is 98, for 10.\", but the best bid is 99, for 10\n");
}

// A replay that isn't the scenario's is refused, not checked as if it were.
TEST(CheckScenario, RefusesAReplayThatIsntTheScenarios) {
    TwoSteps t = two_steps();
    t.steps.steps[1].request = encode_request({.client = 1, .action = Cancel{.id = 10}}, 2);
    EXPECT_NE(check_scenario(t.scenario, t.updates, t.steps).error().find("step 2 isn't the scenario's request"), std::string::npos);
    t = two_steps();
    t.steps.steps.pop_back();
    EXPECT_NE(check_scenario(t.scenario, t.updates, t.steps).error().find("1 steps and the scenario 2"), std::string::npos);
    t = two_steps();
    t.steps.steps[1].first_update = 3;
    EXPECT_NE(check_scenario(t.scenario, t.updates, t.steps).error().find("start at 3"), std::string::npos);
    t = two_steps();
    t.steps.steps[1].update_count = 2;
    EXPECT_NE(check_scenario(t.scenario, t.updates, t.steps).error().find("start at 2, for 2"), std::string::npos);
    t = two_steps();
    t.updates.push_back({.seq = 3, .type = UpdateType::cancel, .order_id = 1, .ticker = 2, .side = Side::buy, .price = 99, .qty = 0, .priority = 1});
    EXPECT_NE(check_scenario(t.scenario, t.updates, t.steps).error().find("made 3 updates, and its steps account for 2"),
              std::string::npos);
}

TEST_F(WireFiles, AFingerprintChangesWithAnyByte) {
    write(dir_ / "m.dat", "LLMDAT01abc");
    const auto first = fingerprint_file(dir_ / "m.dat");
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->starts_with("fnv1a64:"));
    EXPECT_EQ(fingerprint_file(dir_ / "m.dat"), first);
    write(dir_ / "m.dat", "LLMDAT01abd");
    EXPECT_NE(fingerprint_file(dir_ / "m.dat"), first);
    EXPECT_FALSE(fingerprint_file(dir_ / "missing.dat"));
}

TEST(AnswerJson, CarriesTheScenarioAsWrittenAndEachStep) {
    const TwoSteps t = two_steps();
    const auto checked = check_scenario(t.scenario, t.updates, t.steps);
    ASSERT_TRUE(checked);
    Json scenario_json = maker_and_taker();
    const auto json = answer_json(t.scenario, scenario_json, *checked, "/runs/market.dat", "fnv1a64:0123");
    EXPECT_EQ(json["version"], 1);
    EXPECT_EQ(json["question"], t.scenario.question);
    EXPECT_EQ(json["clients"]["1"], "the maker");
    EXPECT_EQ(json["replay"]["marketDataFile"], "/runs/market.dat");
    EXPECT_EQ(json["replay"]["marketDataFingerprint"], "fnv1a64:0123");
    EXPECT_EQ(Json(json["scenario"]), scenario_json);
    ASSERT_EQ(json["steps"].size(), 2u);
    const auto& step = json["steps"][0];
    EXPECT_EQ(step["number"], 1);
    EXPECT_EQ(step["request"], "The maker sends order 10: buy 10 at 99.");
    EXPECT_EQ(step["say"], "The maker bids.");
    EXPECT_EQ(step["updates"]["first"], 1);
    EXPECT_EQ(step["updates"]["count"], 1);
    EXPECT_EQ(step["responses"][0]["to"], "the maker");
    EXPECT_EQ(step["responses"][0]["says"], "order 10 accepted: buy 10 at 99");
    EXPECT_EQ(step["claims"][0]["says"], "The best bid is 99, for 10.");
    EXPECT_EQ(step["claims"][0]["holds"], true);
}

}  // namespace
}  // namespace fillscope
