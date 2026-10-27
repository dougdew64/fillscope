// The book's market maker and liquidity taker (backend/, trading/strategy/ there), run against the
// real exchange beside its random client, and every order each sends checked against its rules,
// given the market data it had received; and the replay's truth, who sent each order and each side
// of each trade, checked against the recording's requests.
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
//   strategy_system_tests EXCHANGE_MAIN TRADING_MAIN EXCHANGE_REPLAY
#include "answer/wire.h"
#include "feed/book_builder.h"
#include "feed/feed_socket.h"
#include "feed/market_data_file.h"
#include "feed/market_update.h"
#include "support/test_support.h"
#include "truth/truth.h"

#include <gtest/gtest.h>

#include <sys/resource.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fillscope {
namespace {

std::string g_exchange, g_trading, g_replay;

const std::uint16_t kOrderPort = test::test_port(0);
const Stream kIncremental{kExchangeIncremental.group, test::test_port(1)};
const Stream kSnapshot{kExchangeSnapshot.group, test::test_port(2)};

std::vector<std::string> with_endpoints(std::vector<std::string> args) {
    for (std::string arg : {std::string{"--order-port"}, std::to_string(kOrderPort), std::string{"--incremental"},
                            std::format("{}:{}", kIncremental.group, kIncremental.port), std::string{"--snapshot"},
                            std::format("{}:{}", kSnapshot.group, kSnapshot.port)}) {
        args.push_back(std::move(arg));
    }
    return args;
}

// A strategy's settings for one ticker, as trading_main takes them: its clip, its threshold, and its
// risk limits (the largest order, the largest position, the loss at which it stops).
struct TickerSettings {
    std::uint32_t clip = 0;
    double threshold = 0;
    std::uint32_t max_order = 0;
    std::uint32_t max_position = 0;
    double max_loss = 0;
};

// The book's own (backend/Chapter12/scripts/run_clients.sh): its client 1, a market maker, and its
// client 3, a liquidity taker, on all eight tickers.
constexpr std::uint32_t kMaker = 1;
constexpr std::uint32_t kTaker = 3;
constexpr std::uint32_t kRandom = 5;
constexpr std::array<TickerSettings, 8> kMakerSettings{{{100, 0.6, 150, 300, -100}, {60, 0.6, 150, 300, -100},
                                                       {150, 0.5, 250, 600, -100}, {200, 0.4, 500, 3000, -100},
                                                       {1000, 0.9, 5000, 4000, -100}, {300, 0.8, 1500, 3000, -100},
                                                       {50, 0.7, 150, 300, -100}, {100, 0.3, 250, 300, -100}}};
constexpr std::array<TickerSettings, 8> kTakerSettings{{{300, 0.8, 350, 300, -300}, {60, 0.7, 350, 300, -300},
                                                       {350, 0.5, 250, 600, -300}, {200, 0.6, 500, 3000, -300},
                                                       {3000, 0.5, 5000, 4000, -300}, {300, 0.7, 3500, 3000, -300},
                                                       {50, 0.3, 350, 300, -300}, {300, 0.8, 350, 300, -300}}};

std::vector<std::string> client(std::uint32_t id, const char* algo, const std::array<TickerSettings, 8>* settings) {
    std::vector<std::string> args{g_trading, std::to_string(id), algo};
    if (settings != nullptr) {
        for (const TickerSettings& s : *settings) {
            for (std::string arg : {std::to_string(s.clip), std::format("{}", s.threshold), std::to_string(s.max_order),
                                    std::to_string(s.max_position), std::format("{}", s.max_loss)}) {
                args.push_back(std::move(arg));
            }
        }
    }
    args.insert(args.end(), {"--warm-up-seconds", "1"});
    return with_endpoints(std::move(args));
}

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

// How many requests each client has in the recording so far: 8 bytes of header, then 30 a request.
std::map<std::uint32_t, std::size_t> recorded_by_client(const std::filesystem::path& recording) {
    std::ifstream in{recording, std::ios::binary};
    std::map<std::uint32_t, std::size_t> counts;
    std::vector<char> bytes{std::istreambuf_iterator<char>{in}, {}};
    for (std::size_t at = 8; at + kRequestSize <= bytes.size(); at += kRequestSize) {
        std::uint32_t client = 0;
        std::memcpy(&client, bytes.data() + at + 1, sizeof client);
        ++counts[client];
    }
    return counts;
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
std::optional<Wanted> maker_wants(const Moment& m, const std::array<TickerSettings, 8>& settings) {
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
std::optional<Wanted> taker_wants(const Moment& m, const std::array<TickerSettings, 8>& settings) {
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

using Rule = std::optional<Wanted> (*)(const Moment&, const std::array<TickerSettings, 8>&);

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
void check_client(std::uint32_t client, Rule rule, const std::array<TickerSettings, 8>& settings,
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

// A session of every kind of client against the real exchange, recorded and replayed once for all
// the tests: the market data, what each request did, and the truth.
struct Session {
    std::vector<MarketUpdate> updates;
    StepsFile steps;
    std::optional<Truth> truth;
};

// (A function with GoogleTest's ASSERTs returns nothing, so it fills in `session`.)
void record(Session& session) {
    const test::TemporaryDirectory temporary{"fillscope-strategies"};
    const std::filesystem::path& directory = temporary.path;
    ASSERT_FALSE(test::tcp_port_open(kOrderPort)) << "something is already listening on this test's order port, " << kOrderPort;
    {
        test::Program exchange(with_endpoints({g_exchange, "--record", "requests.rec"}), directory);
        ASSERT_TRUE(exchange.started());
        const auto exchange_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!test::tcp_port_open(kOrderPort)) {
            ASSERT_TRUE(exchange.running()) << "the exchange exited";
            ASSERT_LT(std::chrono::steady_clock::now(), exchange_deadline) << "the exchange didn't open its order port";
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        test::Program maker(client(kMaker, "MAKER", &kMakerSettings), directory);
        test::Program taker(client(kTaker, "TAKER", &kTakerSettings), directory);
        test::Program random(client(kRandom, "RANDOM", nullptr), directory);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{90};
        for (;;) {
            const auto counts = recorded_by_client(directory / "requests.rec");
            const auto count = [&](std::uint32_t id) { return counts.contains(id) ? counts.at(id) : 0; };
            if (count(kMaker) >= kMakerRequestsWanted && count(kTaker) >= kTakerRequestsWanted) {
                break;
            }
            for (const test::Program* program : {&exchange, &maker, &taker, &random}) {
                ASSERT_TRUE(program->running()) << "a program exited";
            }
            ASSERT_LT(std::chrono::steady_clock::now(), deadline)
                << std::format("after 90 s, the maker had sent {} requests and the taker {}", count(kMaker), count(kTaker));
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
        }
    }  // all killed: the recording holds every request the matcher took

    ASSERT_EQ(test::run_to_exit({g_replay, "requests.rec", "market.dat", "--steps", "steps.bin", "--truth", "truth.bin"}, directory),
              0);
    auto market = read_market_data_file(directory / "market.dat");
    ASSERT_TRUE(market) << market.error();
    auto steps = read_steps_file(directory / "steps.bin");
    ASSERT_TRUE(steps) << steps.error();
    auto truth = read_truth_file(directory / "truth.bin");
    ASSERT_TRUE(truth) << truth.error();
    session.updates = std::move(market->updates);
    session.steps = std::move(*steps);
    session.truth = std::move(*truth);
}

class RecordedSession : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        session_ = new Session;
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
    check_client(kMaker, maker_wants, kMakerSettings, moments, session_->steps, maker);
    check_client(kTaker, taker_wants, kTakerSettings, moments, session_->steps, taker);
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

}  // namespace
}  // namespace fillscope

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc != 4) {
        std::fprintf(stderr, "usage: strategy_system_tests EXCHANGE_MAIN TRADING_MAIN EXCHANGE_REPLAY\n");
        return 2;
    }
    fillscope::g_exchange = std::filesystem::absolute(argv[1]).string();
    fillscope::g_trading = std::filesystem::absolute(argv[2]).string();
    fillscope::g_replay = std::filesystem::absolute(argv[3]).string();
    // The lowest priority, for this process and the programs it starts: the exchange and the clients
    // keep their threads spinning (the book's design, for latency), four programs' worth, and at the
    // usual priority they starved the tests running beside them, which timed out (measured: the
    // sanitizer build's unit and UI tests, eight at a time in each build, as the gate runs them).
    // What this test checks doesn't depend on how fast they run.
    if (::setpriority(PRIO_PROCESS, 0, 19) != 0) {
        std::perror("strategy_system_tests: setpriority");
        return 2;
    }
    return RUN_ALL_TESTS();
}
