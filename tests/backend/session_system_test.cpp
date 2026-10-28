// A session on the real exchange of every kind of client (backend/, trading/strategy/ there): the
// book's market maker and liquidity taker beside its random client, and the planted slicer, working
// an order of each kind, recorded by tools/sessions/record_session.py and replayed. Every order the
// maker and the taker send is checked against their rules, given the market data they had received;
// each sliced order against its plan; the replay's truth, who sent each order and each side of each trade, against the
// recording's requests; the scorer's targets (score/score.h) against the recording's requests too;
// and the replay against itself, done again.
//
// The exchange records every request in the order its matcher took them; replayed, the recording
// gives the market data and, for each request, where in the market data it came. A client decides
// only when a market update arrives, and sends in the order it decides, so each of its requests was
// decided at an update before the request's own, and its decisions never go back. So for each
// request, in order, the check looks for the first update from the last one used, up to the
// request's, at which the strategy's rule gives exactly that request; the market as it was then is
// rebuilt by Fillscope's own books (feed/book_builder.h), not the client's. The rules, from the
// book's code:
//   - The market maker, after any add, modify or cancel on a ticker with both sides quoted, wants a
//     bid and an ask of its clip there. Its fair price is the best bid and ask, each weighted by the
//     other side's quantity; it bids at the best bid if the fair price is at least its threshold
//     above it, a tick lower otherwise, and asks at the best ask, or a tick higher, the same way.
//   - The liquidity taker, after a trade on a ticker with both sides quoted, compares the trade's
//     quantity with the best quantity on the side it took from; at its threshold or more, it wants
//     to trade its clip on the trade's side, at the best price on the other, and nothing on the
//     other side.
// Wanting an order at a price, each keeps one order a side: it cancels one at another price, and
// once that is gone, sends a new one; a risk check may hold the new one back. So each new order is
// at the rule's price for some moment, with the clip, and each cancel is of an order at a price the
// rule no longer wanted.
//
//   session_system_tests PYTHON RECORD_SESSION EXCHANGE_MAIN TRADING_MAIN EXCHANGE_REPLAY
#include "answer/wire.h"
#include "feed/book_builder.h"
#include "feed/feed_socket.h"
#include "feed/market_data_file.h"
#include "feed/market_update.h"
#include "score/score.h"
#include "support/test_support.h"
#include "truth/truth.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fillscope {
namespace {

std::string g_python, g_record_session, g_exchange, g_trading, g_replay;

const std::uint16_t kOrderPort = test::test_port(0);
const Stream kIncremental{kExchangeIncremental.group, test::test_port(1)};
const Stream kSnapshot{kExchangeSnapshot.group, test::test_port(2)};

// A strategy's settings for one ticker, as trading_main takes them: its clip, its threshold, and its
// risk limits (the largest order, the largest position, the loss at which it stops).
struct TickerSettings {
    std::uint32_t clip = 0;
    double threshold = 0;
    std::uint32_t max_order = 0;
    std::uint32_t max_position = 0;
    double max_loss = 0;
};
using Settings = std::array<TickerSettings, 8>;

// The clients the session runs (record_session.py): the book's client 1, a market maker, and its
// client 3, a liquidity taker, with its own settings (session.json says them); its random client 5;
// and the slicer, client 6.
constexpr std::uint32_t kMaker = 1;
constexpr std::uint32_t kTaker = 3;
constexpr std::uint32_t kRandom = 5;

// A request, as the recording and the steps file hold it (wire.h has the layout).
struct Request {
    std::uint8_t type = 0;  // 1, a new order; 2, a cancel
    std::uint32_t client = 0;
    std::uint32_t ticker = 0;
    std::uint64_t order_id = 0;  // the client's
    Side side = Side::none;
    std::int64_t price = 0;
    std::uint32_t qty = 0;
};
constexpr std::uint8_t kNew = 1;
constexpr std::uint8_t kCancel = 2;

Request decode(const RequestBytes& bytes) {
    Request request;
    std::size_t at = 0;
    const auto take = [&](auto& field) {
        std::memcpy(&field, bytes.data() + at, sizeof field);
        at += sizeof field;
    };
    std::int8_t side = 0;
    take(request.type);
    take(request.client);
    take(request.ticker);
    take(request.order_id);
    take(side);
    take(request.price);
    take(request.qty);
    request.side = side == 1 ? Side::buy : side == -1 ? Side::sell : Side::none;
    return request;
}

// The market as a client saw it after one update: the update, and its ticker's best bid and ask,
// each with the whole quantity at that price (none: that side is empty).
struct Moment {
    MarketUpdate update;
    std::optional<std::int64_t> bid, ask;
    std::uint64_t bid_qty = 0, ask_qty = 0;
};

// Every moment, indexed by update number (moments[0] is before any). A trade changes no book, so its
// moment's best prices are those the trade met.
std::vector<Moment> moments_of(const std::vector<MarketUpdate>& updates) {
    std::vector<Moment> moments(updates.size() + 1);
    BookBuilder books;
    for (const MarketUpdate& update : updates) {
        books.on_incremental(update);
        EXPECT_EQ(books.status(), BookBuilder::Status::live) << "update " << update.seq;
        Moment& moment = moments[update.seq];
        moment.update = update;
        const Book& book = books.book(update.ticker);
        if (const auto bids = book.levels(Side::buy); !bids.empty()) {
            moment.bid = bids.front().price;
            moment.bid_qty = bids.front().qty;
        }
        if (const auto asks = book.levels(Side::sell); !asks.empty()) {
            moment.ask = asks.front().price;
            moment.ask_qty = asks.front().qty;
        }
    }
    return moments;
}

// What a strategy wants on one ticker after one moment: an order of its clip at a price on each
// side, or none there (nullopt). Nothing at all if the moment doesn't move it.
struct Wanted {
    std::optional<std::int64_t> bid, ask;
};

// The market maker's rule (trading/strategy/market_maker.h there): the same arithmetic in doubles.
std::optional<Wanted> maker_wants(const Moment& m, const Settings& settings) {
    if (m.update.type == UpdateType::trade || !m.bid || !m.ask) {
        return std::nullopt;
    }
    const double fair = static_cast<double>(*m.bid * static_cast<std::int64_t>(m.ask_qty) + *m.ask * static_cast<std::int64_t>(m.bid_qty)) /
                        static_cast<double>(m.bid_qty + m.ask_qty);
    const double threshold = settings.at(m.update.ticker).threshold;
    return Wanted{.bid = *m.bid - (fair - static_cast<double>(*m.bid) >= threshold ? 0 : 1),
                  .ask = *m.ask + (static_cast<double>(*m.ask) - fair >= threshold ? 0 : 1)};
}

// The liquidity taker's (trading/strategy/liquidity_taker.h there): a trade's side is that of the
// order that came in and took the other side's.
std::optional<Wanted> taker_wants(const Moment& m, const Settings& settings) {
    if (m.update.type != UpdateType::trade || !m.bid || !m.ask) {
        return std::nullopt;
    }
    const bool bought = m.update.side == Side::buy;
    const double ratio = static_cast<double>(m.update.qty) / static_cast<double>(bought ? m.ask_qty : m.bid_qty);
    if (ratio < settings.at(m.update.ticker).threshold) {
        return std::nullopt;
    }
    return bought ? Wanted{.bid = *m.ask, .ask = std::nullopt} : Wanted{.bid = std::nullopt, .ask = *m.bid};
}

using Rule = std::optional<Wanted> (*)(const Moment&, const Settings&);

struct Checked {
    std::size_t new_orders = 0;
    std::size_t cancels = 0;
    std::set<std::uint32_t> tickers;  // those it sent a new order on
    std::size_t fills = 0;            // fills of its orders, said to it
};

// Each of `client`'s requests, in order, against `rule`: a new order of its clip at the price the
// rule wanted on its side, or a cancel of an order at a price the rule no longer wanted there, at a
// moment from the last one used up to the request's own.
// (A function with GoogleTest's ASSERTs returns nothing, so it fills in `checked`.)
void check_client(std::uint32_t client, Rule rule, const Settings& settings,
                  const std::vector<Moment>& moments, const StepsFile& steps, Checked& checked) {
    std::map<std::uint64_t, Request> sent;  // its new orders, by its order id
    std::uint64_t from = 1;                  // the earliest moment its next request can have come from
    for (const ReplayStep& step : steps.steps) {
        for (const ClientResponse& response : step.responses) {
            if (response.client == client && response.type == ResponseType::filled) {
                ++checked.fills;
            }
        }
        const Request request = decode(step.request);
        if (request.client != client) {
            continue;
        }
        const std::uint64_t until = step.first_update - 1;  // the last update made before it
        std::optional<Request> cancelled;
        if (request.type == kNew) {
            EXPECT_EQ(request.qty, settings.at(request.ticker).clip)
                << std::format("client {}'s order {}: not its clip on ticker {}", client, request.order_id, request.ticker);
            sent[request.order_id] = request;
        } else {
            ASSERT_EQ(request.type, kCancel) << std::format("client {} sent a request of type {}", client, request.type);
            const auto found = sent.find(request.order_id);
            ASSERT_NE(found, sent.end()) << std::format("client {} cancelled order {}, which it never sent", client, request.order_id);
            cancelled = found->second;
        }
        const Request& order = cancelled ? *cancelled : request;
        std::optional<std::uint64_t> at;
        for (std::uint64_t k = from; k <= until && !at; ++k) {
            if (moments[k].update.ticker != order.ticker) {
                continue;
            }
            const auto wanted = rule(moments[k], settings);
            if (!wanted) {
                continue;
            }
            const std::optional<std::int64_t> price = order.side == Side::buy ? wanted->bid : wanted->ask;
            if (cancelled ? price != order.price : price == order.price) {
                at = k;
            }
        }
        ASSERT_TRUE(at) << std::format(
            "client {}'s {} of order {} (ticker {}, {} {} at {}), sent after update {}: no update from {} on gives it",
            client, cancelled ? "cancel" : "new order", order.order_id, order.ticker, order.side == Side::buy ? "buy" : "sell",
            order.qty, order.price, until, from);
        from = *at;
        if (cancelled) {
            ++checked.cancels;
        } else {
            ++checked.new_orders;
            checked.tickers.insert(order.ticker);
        }
    }
}

// Enough of each strategy's requests for the check to mean something: the taker trades only now and
// then, when a trade is large against the book.
constexpr std::size_t kMakerRequestsWanted = 300;
constexpr std::size_t kTakerRequestsWanted = 20;

// An order the slicer worked, as the session ran it: the order, and the plan the slicer wrote as it
// started, each piece's quantity and the updates on its ticker it waited for before it.
struct SlicedOrder {
    std::uint32_t client = 0;  // the slicer's
    std::string kind;
    std::uint32_t ticker = 0;
    Side side = Side::none;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> plan;
    bool recovered = false;  // it recovered from a snapshot, counting the snapshot's orders as updates
};

// A session of every kind of client against the real exchange, recorded and replayed once for all
// the tests: the market data, what each request did, the truth, and who ran with what settings.
struct Session {
    test::TemporaryDirectory directory{"fillscope-session"};
    std::vector<MarketUpdate> updates;
    StepsFile steps;
    std::optional<Truth> truth;
    Settings maker{};
    Settings taker{};
    std::vector<SlicedOrder> sliced;
};

Settings settings_of(const nlohmann::json& client) {
    Settings settings{};
    EXPECT_EQ(client.at("tickers").size(), settings.size()) << client.dump();
    for (std::size_t i = 0; i < settings.size() && i < client.at("tickers").size(); ++i) {
        const auto& t = client.at("tickers")[i];
        settings[i] = {t.at("clip"), t.at("threshold"), t.at("max_order"), t.at("max_position"), t.at("max_loss")};
    }
    return settings;
}

// (A function with GoogleTest's ASSERTs returns nothing, so it fills in `session`.)
void record(Session& session) {
    const std::filesystem::path& directory = session.directory.path;
    ASSERT_FALSE(test::tcp_port_open(kOrderPort)) << "something is already listening on this test's order port, " << kOrderPort;
    const int recorded = test::run_to_exit(
        {g_python, "-I", g_record_session, directory.string(), "--exchange", g_exchange, "--trading", g_trading, "--replay",
         g_replay, "--order-port", std::to_string(kOrderPort), "--incremental", std::format("{}:{}", kIncremental.group, kIncremental.port),
         "--snapshot", std::format("{}:{}", kSnapshot.group, kSnapshot.port), "--at-least",
         std::format("{}:{}", kMaker, kMakerRequestsWanted), "--at-least", std::format("{}:{}", kTaker, kTakerRequestsWanted),
         "--max-seconds", "100"},
        directory);
    std::ifstream said{directory / "record_session.log"};
    const std::string log{std::istreambuf_iterator<char>{said}, {}};
    std::printf("%s", log.c_str());
    ASSERT_EQ(recorded, 0) << "record_session.py: " << log;

    auto market = read_market_data_file(directory / "market.dat");
    ASSERT_TRUE(market) << market.error();
    auto steps = read_steps_file(directory / "steps.bin");
    ASSERT_TRUE(steps) << steps.error();
    auto truth = read_truth_file(directory / "truth.bin");
    ASSERT_TRUE(truth) << truth.error();
    session.updates = std::move(market->updates);
    session.steps = std::move(*steps);
    session.truth = std::move(*truth);

    std::ifstream in{directory / "session.json"};
    const auto json = nlohmann::json::parse(in);
    // A client that recovered from a snapshot decided, as it rebuilt its book, on books half built,
    // and a slicer that did counted the snapshot's orders as updates: said, so a failure explains
    // itself.
    std::string recovered;
    for (const auto& client : json.at("clients")) {
        if (client.at("recovered")) {
            recovered += std::format(" {}", client.at("client").get<std::uint32_t>());
        }
    }
    std::printf("clients that recovered from a snapshot:%s\n", recovered.empty() ? " none" : recovered.c_str());
    for (const auto& client : json.at("clients")) {
        const std::string strategy = client.at("strategy");
        if (strategy == "MAKER") {
            ASSERT_EQ(client.at("client"), kMaker);
            session.maker = settings_of(client);
        } else if (strategy == "TAKER") {
            ASSERT_EQ(client.at("client"), kTaker);
            session.taker = settings_of(client);
        } else if (strategy == "SLICER") {
            for (const auto& order : client.at("orders")) {
                SlicedOrder sliced{.client = client.at("client"), .kind = order.at("kind"), .ticker = order.at("ticker"),
                                   .side = order.at("side") == "BUY" ? Side::buy : Side::sell, .plan = {},
                                   .recovered = client.at("recovered")};
                for (const auto& piece : order.at("plan")) {
                    sliced.plan.emplace_back(piece.at(0), piece.at(1));
                }
                session.sliced.push_back(std::move(sliced));
            }
        }
    }
    ASSERT_EQ(session.sliced.size(), 3u) << "an order of each kind";
}

class RecordedSession : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        session_ = new Session;  // its directory stays until the last test is done
        record(*session_);
        if (::testing::Test::HasFailure()) {
            delete session_;
            session_ = nullptr;
        }
    }
    static void TearDownTestSuite() {
        delete session_;
        session_ = nullptr;
    }
    void SetUp() override { ASSERT_NE(session_, nullptr) << "the session wasn't recorded"; }

    static Session* session_;
};
Session* RecordedSession::session_ = nullptr;

TEST_F(RecordedSession, EveryOrderTheMarketMakerAndTheLiquidityTakerSendFollowsTheirRules) {
    const std::vector<Moment> moments = moments_of(session_->updates);
    Checked maker;
    Checked taker;
    check_client(kMaker, maker_wants, session_->maker, moments, session_->steps, maker);
    check_client(kTaker, taker_wants, session_->taker, moments, session_->steps, taker);
    // Not vacuous: each strategy traded, the maker on several tickers, moving its quotes.
    EXPECT_GE(maker.new_orders, 100u);
    EXPECT_GE(maker.cancels, 50u);
    EXPECT_GE(maker.tickers.size(), 4u);
    EXPECT_GE(taker.new_orders, 10u);
    EXPECT_GE(taker.fills, 1u);
    std::printf("checked: the maker's %zu new orders on %zu tickers and %zu cancels (%zu fills); the taker's %zu new orders "
                "and %zu cancels (%zu fills)\n",
                maker.new_orders, maker.tickers.size(), maker.cancels, maker.fills, taker.new_orders, taker.cancels, taker.fills);
}

// The truth against the recording's requests, worked out here without the matcher's responses, from
// which the replay wrote it: the matcher numbers each ticker's orders from 1 in the order it accepts
// them, and accepts every new order; a request's own updates are those its step says it made; and a
// trade is followed by its resting order's update, its cancel if the trade filled it, its modify if
// not (exchange/matcher/me_order_book.cpp there, match()).
TEST_F(RecordedSession, EveryOrderAndEveryTradeHasItsClientInTheTruth) {
    const Truth& truth = *session_->truth;
    EXPECT_EQ(truth.recording(), "requests.rec");
    std::map<std::uint32_t, std::uint64_t> last_id;                          // by ticker
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> sent;  // by ticker and order id: its client
    std::size_t orders = 0;
    std::size_t trades = 0;
    std::map<std::uint32_t, std::size_t> traded;  // by client: the trades it was on either side of
    const std::vector<MarketUpdate>& updates = session_->updates;
    for (const ReplayStep& step : session_->steps.steps) {
        const Request request = decode(step.request);
        std::optional<std::uint64_t> new_id;
        if (request.type == kNew) {
            new_id = ++last_id[request.ticker];
            sent[{request.ticker, *new_id}] = request.client;
            ASSERT_LT(orders, truth.orders().size()) << "the truth has fewer orders than the recording";
            EXPECT_EQ(truth.orders()[orders], (OrderTruth{.ticker = request.ticker, .order_id = *new_id, .client = request.client}))
                << "order " << orders + 1;
            ++orders;
        }
        const std::uint64_t end = step.first_update + step.update_count;
        for (std::uint64_t seq = step.first_update; seq < end; ++seq) {
            const MarketUpdate& update = updates.at(seq - 1);
            if (update.type == UpdateType::add) {
                EXPECT_EQ(truth.client_of(update.ticker, update.order_id), request.client) << "the order added at update " << seq;
            } else if (update.type == UpdateType::trade) {
                ++trades;
                const TradeTruth* trade = truth.trade_at(seq);
                ASSERT_NE(trade, nullptr) << "the trade at update " << seq << " isn't in the truth";
                ASSERT_TRUE(new_id) << "the trade at update " << seq << " was made by a cancel";
                EXPECT_EQ(trade->incoming_client, request.client) << "update " << seq;
                EXPECT_EQ(trade->incoming_order_id, *new_id) << "update " << seq;
                ASSERT_LT(seq + 1, end) << "the trade at update " << seq << " is its request's last update";
                const MarketUpdate& resting = updates.at(seq);
                ASSERT_TRUE(resting.type == UpdateType::cancel || resting.type == UpdateType::modify) << "update " << seq + 1;
                ASSERT_EQ(resting.ticker, update.ticker) << "update " << seq + 1;
                EXPECT_EQ(trade->resting_order_id, resting.order_id) << "update " << seq;
                ASSERT_TRUE(sent.contains({resting.ticker, resting.order_id})) << "update " << seq + 1;
                EXPECT_EQ(trade->resting_client, sent.at({resting.ticker, resting.order_id})) << "update " << seq;
                ++traded[trade->incoming_client];
                if (trade->resting_client != trade->incoming_client) {
                    ++traded[trade->resting_client];
                }
            }
        }
    }
    EXPECT_EQ(truth.orders().size(), orders) << "the truth has orders the recording doesn't";
    EXPECT_EQ(truth.trades().size(), trades) << "the truth has trades the market data doesn't";
    // Not vacuous: every kind of client traded.
    for (const std::uint32_t client : {kMaker, kTaker, kRandom}) {
        EXPECT_GE(traded[client], 1u) << "client " << client << " never traded";
    }
    std::printf("checked the truth: %zu orders and %zu trades (on either side: the maker %zu, the taker %zu, the random "
                "client %zu)\n",
                orders, trades, traded[kMaker], traded[kTaker], traded[kRandom]);
}

// Each slicer sends exactly its plan: new orders on its ticker and side, numbered 1, 2, 3, ..., each
// of its piece's quantity, every piece and no more, and a cancel of each once it was accepted. And each piece came its gap of updates on
// the ticker after the one before: the slicer counts the updates as it receives them, and a piece
// reaches the matcher some updates after the slicer decided on it. So if piece i was decided at the
// ticker's k-th update, k is a fixed start plus the sum of the gaps up to i, and no later than the
// ticker's updates before the piece arrived, a(i). So a(i) less the gaps' sum is, for every piece,
// that start plus the updates the piece was on its way. How many depends on how busy the machine
// is (up to 6 measured, at the lowest priority), but a late piece doesn't delay the next, so with
// the plan's gaps the least of them is the same at the order's end as at its start; a slicer that
// miscounted its gaps would drift, a whole update a piece or more. (That each piece is priced at the
// best price opposite is the fork's own test, which can see the slicer's books.)
constexpr std::size_t kPiecesCompared = 6;  // at each end of the order
constexpr std::uint64_t kMostDrift = 2;

TEST_F(RecordedSession, EachSlicedOrderIsSentExactlyAsPlannedInItsPiecesAndGaps) {
    for (const SlicedOrder& slicer : session_->sliced) {
        SCOPED_TRACE(std::format("the slicer's order on ticker {} ({})", slicer.ticker, slicer.kind));
        ASSERT_GE(slicer.plan.size(), 10u);
        std::uint64_t on_ticker = 0;  // the ticker's updates so far
        std::uint64_t seq = 0;        // the last update counted
        std::uint64_t gaps = 0;       // the gaps' sum so far
        std::vector<std::uint64_t> late;  // each piece's a(i) less the gaps' sum
        std::set<std::uint64_t> cancelled;
        std::size_t piece = 0;
        for (const ReplayStep& step : session_->steps.steps) {
            for (; seq + 1 < step.first_update; ++seq) {  // the updates made before this request
                on_ticker += session_->updates.at(seq).ticker == slicer.ticker;
            }
            const Request request = decode(step.request);
            if (request.client != slicer.client || request.ticker != slicer.ticker) {
                continue;
            }
            if (request.type == kCancel) {  // each piece, once accepted: one cancel each, after it
                EXPECT_TRUE(request.order_id >= 1 && request.order_id <= piece) << "a cancel of order " << request.order_id;
                EXPECT_TRUE(cancelled.insert(request.order_id).second) << "order " << request.order_id << " cancelled twice";
                continue;
            }
            ASSERT_LT(piece, slicer.plan.size()) << "a request past its plan";
            EXPECT_EQ(request.type, kNew) << "piece " << piece + 1;
            EXPECT_EQ(request.side, slicer.side) << "piece " << piece + 1;
            EXPECT_EQ(request.order_id, piece + 1);
            EXPECT_EQ(request.qty, slicer.plan[piece].first) << "piece " << piece + 1;
            gaps += slicer.plan[piece].second;
            ASSERT_GE(on_ticker, gaps) << "piece " << piece + 1 << " came before its gaps had passed";
            late.push_back(on_ticker - gaps);
            ++piece;
        }
        ASSERT_EQ(piece, slicer.plan.size()) << "every piece sent";
        EXPECT_EQ(cancelled.size(), piece) << "every piece cancelled";
        ASSERT_GE(late.size(), 2 * kPiecesCompared);
        const auto least = [](auto first, auto last) { return *std::min_element(first, last); };
        const std::uint64_t start = least(late.begin(), late.begin() + kPiecesCompared);
        const std::uint64_t end = least(late.end() - kPiecesCompared, late.end());
        const std::uint64_t base = std::min(start, end);
        std::string lags;
        for (const std::uint64_t l : late) {
            lags += std::format(" {}", l - least(late.begin(), late.end()));
        }
        EXPECT_LE(std::max(start, end) - base, kMostDrift)
            << "the pieces' gaps aren't the plan's: the updates each was late by,"
            << lags << (slicer.recovered ? " (it recovered from a snapshot, whose orders it counted)" : "");
        std::printf("the slicer's order on ticker %u (%s): %zu pieces, each its gap after the last; the updates each was "
                    "late by:%s\n",
                    slicer.ticker, slicer.kind.c_str(), piece, lags.c_str());
    }
}

// The trades `clients` were on a side of, as the recording says, worked out without the truth: those
// whose incoming order one of their requests sent, or whose resting order one of their requests added.
std::set<std::uint64_t> trades_of(const Session& session, const std::set<std::uint32_t>& clients) {
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> added_by;  // an order on the book: who added it
    std::set<std::uint64_t> trades;
    for (const ReplayStep& step : session.steps.steps) {
        const Request request = decode(step.request);
        for (std::uint64_t seq = step.first_update; seq < step.first_update + step.update_count; ++seq) {
            const MarketUpdate& update = session.updates.at(seq - 1);
            if (update.type == UpdateType::add) {
                added_by[{update.ticker, update.order_id}] = request.client;
            } else if (update.type == UpdateType::trade) {
                const MarketUpdate& resting = session.updates.at(seq);  // its resting order's cancel or modify
                if (clients.contains(request.client) || clients.contains(added_by.at({resting.ticker, resting.order_id}))) {
                    trades.insert(seq);
                }
            }
        }
    }
    return trades;
}

std::set<std::uint32_t> slicers_of(const Session& session) {
    std::set<std::uint32_t> slicers;
    for (const SlicedOrder& order : session.sliced) {
        slicers.insert(order.client);
    }
    return slicers;
}

// The truth marks exactly the slicer's trades: those it says the slicer was on a side of are those
// the recording says it was (trades_of).
TEST_F(RecordedSession, TheTruthMarksExactlyTheSlicersTrades) {
    const std::set<std::uint32_t> slicers = slicers_of(*session_);
    const std::set<std::uint64_t> slicers_trades = trades_of(*session_, slicers);
    std::map<std::uint32_t, std::size_t> traded;  // the slicer's trades, by ticker
    for (const std::uint64_t seq : slicers_trades) {
        ++traded[session_->updates.at(seq - 1).ticker];
    }
    std::set<std::uint64_t> marked;
    for (const TradeTruth& trade : session_->truth->trades()) {
        if (slicers.contains(trade.incoming_client) || slicers.contains(trade.resting_client)) {
            marked.insert(trade.update);
        }
    }
    EXPECT_EQ(marked, slicers_trades);
    std::string by_ticker;
    for (const SlicedOrder& order : session_->sliced) {
        EXPECT_GE(traded[order.ticker], 5u) << "the slicer hardly traded on ticker " << order.ticker;
        by_ticker += std::format(" {} on ticker {} ({}),", traded[order.ticker], order.ticker, order.kind);
    }
    std::printf("the truth marks %zu trades as the slicer's:%s\n", marked.size(), by_ticker.c_str());
}

// The scorer's targets (score/score.h) are the slicer and the taker, read from session.json, and the
// trades it scores as theirs are those the recording says they were on a side of (trades_of). So
// the targets' trades flagged exactly score perfectly; nothing flagged misses them all; everything
// flagged falsely alarms on every other trade, the maker's and the random client's with each other.
TEST_F(RecordedSession, TheScorersTargetsAreTheSlicersAndTheTakersTrades) {
    const auto targets = read_targets(session_->directory.path / "session.json");
    ASSERT_TRUE(targets) << targets.error();
    std::set<std::uint32_t> expected = slicers_of(*session_);
    expected.insert(kTaker);
    ASSERT_EQ(*targets, expected);
    const std::set<std::uint64_t> targets_trades = trades_of(*session_, *targets);
    ASSERT_EQ(target_trades(*session_->truth, *targets), targets_trades);

    std::set<std::uint64_t> every_trade;
    for (const MarketUpdate& update : session_->updates) {
        if (update.type == UpdateType::trade) {
            every_trade.insert(update.seq);
        }
    }
    const std::size_t others = every_trade.size() - targets_trades.size();
    EXPECT_GT(others, 0u) << "no trade without a target, so nothing to alarm falsely on";
    const auto score = [&](const std::set<std::uint64_t>& flagged) {
        auto scored = score_flags(session_->updates, *session_->truth, *targets, flagged);
        EXPECT_TRUE(scored) << scored.error();
        return scored.value_or(Score{}).all;
    };
    const std::size_t trades = every_trade.size();
    const std::size_t hit = targets_trades.size();
    EXPECT_EQ(score(targets_trades), (Tally{.trades = trades, .targets = hit, .hits = hit, .misses = 0, .false_alarms = 0}));
    EXPECT_EQ(score({}), (Tally{.trades = trades, .targets = hit, .hits = 0, .misses = hit, .false_alarms = 0}));
    EXPECT_EQ(score(every_trade), (Tally{.trades = trades, .targets = hit, .hits = hit, .misses = 0, .false_alarms = others}));
    std::printf("the scorer's targets: clients%s; %zu of the %zu trades are theirs\n",
                std::accumulate(targets->begin(), targets->end(), std::string{},
                                [](std::string said, std::uint32_t client) { return said + std::format(" {}", client); })
                    .c_str(),
                hit, trades);
}

// The recording, replayed again, gives the same market data, steps and truth, byte for byte.
TEST_F(RecordedSession, ASessionReplaysTheSameEveryTime) {
    const std::filesystem::path& directory = session_->directory.path;
    ASSERT_EQ(test::run_to_exit({g_replay, "requests.rec", "again.dat", "--steps", "again-steps.bin", "--truth", "again-truth.bin"},
                                directory),
              0);
    const auto bytes = [&](const char* name) {
        std::ifstream in{directory / name, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, {}};
    };
    for (const auto& [first, again] : {std::pair{"market.dat", "again.dat"}, std::pair{"steps.bin", "again-steps.bin"},
                                       std::pair{"truth.bin", "again-truth.bin"}}) {
        const std::string original = bytes(first);
        EXPECT_GT(original.size(), 1000u) << first;
        EXPECT_TRUE(original == bytes(again)) << first << " differs, replayed again";
    }
}

}  // namespace
}  // namespace fillscope

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc != 6) {
        std::fprintf(stderr, "usage: session_system_tests PYTHON RECORD_SESSION EXCHANGE_MAIN TRADING_MAIN EXCHANGE_REPLAY\n");
        return 2;
    }
    fillscope::g_python = argv[1];
    fillscope::g_record_session = std::filesystem::absolute(argv[2]).string();
    fillscope::g_exchange = std::filesystem::absolute(argv[3]).string();
    fillscope::g_trading = std::filesystem::absolute(argv[4]).string();
    fillscope::g_replay = std::filesystem::absolute(argv[5]).string();
    // The exchange and the clients keep their threads spinning (the book's design, for latency): five
    // programs, about 8 CPUs' worth. So this process, and the programs it starts, take the lowest
    // priority, and CTest runs it with no other test beside it (tests/backend/CMakeLists.txt says
    // why). What this test checks doesn't depend on how fast they run.
    if (::setpriority(PRIO_PROCESS, 0, 19) != 0) {
        std::perror("session_system_tests: setpriority");
        return 2;
    }
    return RUN_ALL_TESTS();
}
