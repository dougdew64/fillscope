// The exchange's market data as Fillscope reads it: decoding (feed/market_update.h), the books
// (feed/book.h), keeping them in step with the feed and recovering from snapshots
// (feed/book_builder.h), and receiving it (feed/feed_socket.h).
#include "feed/book.h"
#include "feed/book_builder.h"
#include "feed/feed_socket.h"
#include "feed/market_data_file.h"
#include "feed/market_update.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fillscope {
namespace {

using Type = UpdateType;

MarketUpdate add(std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty, std::uint64_t priority,
                 std::uint32_t ticker = 0) {
    return {.seq = seq, .type = Type::add, .order_id = id, .ticker = ticker, .side = side, .price = price, .qty = qty, .priority = priority};
}

MarketUpdate modify(std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty, std::uint32_t ticker = 0) {
    return {.seq = seq, .type = Type::modify, .order_id = id, .ticker = ticker, .side = side, .price = price, .qty = qty, .priority = 0};
}

MarketUpdate cancel(std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price, std::uint32_t ticker = 0) {
    return {.seq = seq, .type = Type::cancel, .order_id = id, .ticker = ticker, .side = side, .price = price, .qty = 0, .priority = 0};
}

MarketUpdate trade(std::uint64_t seq, Side side, std::int64_t price, std::uint32_t qty, std::uint32_t ticker = 0) {
    return {.seq = seq, .type = Type::trade, .order_id = 0, .ticker = ticker, .side = side, .price = price, .qty = qty, .priority = 0};
}

// A whole snapshot, numbered from 0, as the exchange sends it: a start naming the last incremental
// update it includes, a clear for each ticker followed by its orders, an end.
std::vector<MarketUpdate> snapshot(std::uint64_t last_seq, const std::vector<MarketUpdate>& orders) {
    std::vector<MarketUpdate> updates{{.seq = 0, .type = Type::snapshot_start, .order_id = last_seq}};
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        updates.push_back({.type = Type::clear, .ticker = ticker});
        for (const MarketUpdate& order : orders) {
            if (order.ticker == ticker) {
                updates.push_back(order);
            }
        }
    }
    updates.push_back({.type = Type::snapshot_end, .order_id = last_seq});
    for (std::uint64_t i = 0; i < updates.size(); ++i) {
        updates[i].seq = i;
    }
    return updates;
}

void feed_snapshot(BookBuilder& builder, const std::vector<MarketUpdate>& updates) {
    for (const MarketUpdate& update : updates) {
        builder.on_snapshot(update);
    }
}

// ---- Decoding ----

TEST(MarketUpdate, EncodingThenDecodingGivesTheUpdateBack) {
    const MarketUpdate update{.seq = 0x0102030405060708, .type = Type::modify, .order_id = 0x1112131415161718,
                              .ticker = 0x21222324, .side = Side::sell, .price = -2, .qty = 0x31323334,
                              .priority = 0x4142434445464748};
    const auto bytes = encode_update(update);
    const auto decoded = decode_updates(bytes);
    ASSERT_TRUE(decoded) << decoded.error();
    ASSERT_EQ(decoded->size(), 1u);
    EXPECT_EQ(decoded->front(), update);
}

TEST(MarketUpdate, DecodesTheBytesTheExchangeSends) {
    // The bytes of the back end's own test of its layout (tests/market_update_test.cpp there).
    const unsigned char wire[kUpdateSize] = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 3, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x24, 0x23, 0x22, 0x21, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x34, 0x33, 0x32, 0x31,
        0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41};
    const auto decoded = decode_updates(std::as_bytes(std::span{wire}));
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_EQ(decoded->front(), (MarketUpdate{.seq = 0x0102030405060708, .type = Type::modify, .order_id = 0x1112131415161718,
                                              .ticker = 0x21222324, .side = Side::sell, .price = -2, .qty = 0x31323334,
                                              .priority = 0x4142434445464748}));
}

TEST(MarketUpdate, ADatagramMayHoldSeveralUpdates) {
    std::vector<std::byte> datagram;
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        const auto bytes = encode_update(add(seq, seq, Side::buy, 100, 1, seq));
        datagram.insert(datagram.end(), bytes.begin(), bytes.end());
    }
    const auto decoded = decode_updates(datagram);
    ASSERT_TRUE(decoded) << decoded.error();
    ASSERT_EQ(decoded->size(), 3u);
    EXPECT_EQ((*decoded)[2].seq, 3u);
}

TEST(MarketUpdate, RejectsBytesThatArentWholeUpdatesOfKnownTypesAndSides) {
    auto bytes = encode_update(add(1, 1, Side::buy, 100, 1, 1));
    EXPECT_FALSE(decode_updates(std::span{bytes}.first(kUpdateSize - 1)));
    bytes[8] = std::byte{0};  // type 0, INVALID
    EXPECT_FALSE(decode_updates(bytes));
    bytes[8] = std::byte{8};  // past the last type
    EXPECT_FALSE(decode_updates(bytes));
    bytes = encode_update(add(1, 1, Side::buy, 100, 1, 1));
    bytes[21] = std::byte{2};  // side MAX, not a side
    EXPECT_FALSE(decode_updates(bytes));
}

// ---- One book ----

TEST(Book, KeepsEachPricesOrdersInPriorityOrderAndTheBestPriceFirst) {
    Book book;
    for (const auto& update : {add(1, 1, Side::buy, 100, 5, 2), add(2, 2, Side::buy, 101, 3, 1), add(3, 3, Side::buy, 100, 7, 1),
                               add(4, 4, Side::sell, 103, 2, 1), add(5, 5, Side::sell, 102, 4, 1)}) {
        ASSERT_TRUE(book.apply(update)) << update.seq;
    }
    const auto bids = book.levels(Side::buy);
    ASSERT_EQ(bids.size(), 2u);
    EXPECT_EQ(bids[0].price, 101);
    EXPECT_EQ(bids[1].price, 100);
    EXPECT_EQ(bids[1].qty, 12u);
    ASSERT_EQ(bids[1].orders.size(), 2u);
    EXPECT_EQ(bids[1].orders[0].order_id, 3u) << "priority 1 before priority 2, whatever came first";
    const auto asks = book.levels(Side::sell);
    ASSERT_EQ(asks.size(), 2u);
    EXPECT_EQ(asks[0].price, 102);
    EXPECT_EQ(book.order_count(), 5u);
}

TEST(Book, AModifyChangesTheQuantityAndACancelRemovesTheOrderAndAnEmptyLevel) {
    Book book;
    ASSERT_TRUE(book.apply(add(1, 1, Side::sell, 100, 10, 1)));
    ASSERT_TRUE(book.apply(modify(2, 1, Side::sell, 100, 6)));
    EXPECT_EQ(book.find(1)->qty, 6u);
    EXPECT_EQ(book.find(1)->priority, 1u) << "a partial fill keeps the order's place";
    ASSERT_TRUE(book.apply(cancel(3, 1, Side::sell, 100)));
    EXPECT_EQ(book.find(1), nullptr);
    EXPECT_TRUE(book.levels(Side::sell).empty());
    EXPECT_FALSE(book.level(Side::sell, 100));
}

TEST(Book, ATradeChangesNothing) {
    Book book;
    ASSERT_TRUE(book.apply(add(1, 1, Side::sell, 100, 10, 1)));
    ASSERT_TRUE(book.apply(trade(2, Side::buy, 100, 4)));
    EXPECT_EQ(book.find(1)->qty, 10u);
}

TEST(Book, RejectsAnUpdateItCantFollowAndStaysAsItWas) {
    Book book;
    ASSERT_TRUE(book.apply(add(1, 1, Side::buy, 100, 5, 1)));
    const auto before = book.orders();
    EXPECT_FALSE(book.apply(add(2, 1, Side::buy, 99, 5, 1))) << "an order already in the book";
    EXPECT_FALSE(book.apply(add(3, 2, Side::buy, 100, 5, 1))) << "a priority already taken at the price";
    EXPECT_FALSE(book.apply(add(4, 3, Side::none, 100, 5, 2))) << "no side";
    EXPECT_FALSE(book.apply(modify(5, 9, Side::buy, 100, 1))) << "an order not in the book";
    EXPECT_FALSE(book.apply(modify(6, 1, Side::buy, 101, 1))) << "at another price";
    EXPECT_FALSE(book.apply(cancel(7, 9, Side::buy, 100))) << "an order not in the book";
    EXPECT_FALSE(book.apply(cancel(8, 1, Side::sell, 100))) << "on the other side";
    EXPECT_EQ(book.orders(), before);
    EXPECT_EQ(book.levels(Side::buy).size(), 1u) << "no empty level left behind";
}

// ---- Keeping the books in step ----

TEST(BookBuilder, WaitsThenAppliesUpdatesInOrderFromTheFirst) {
    BookBuilder builder;
    EXPECT_EQ(builder.status(), BookBuilder::Status::waiting);
    builder.on_incremental(add(1, 1, Side::buy, 100, 5, 1, 2));
    builder.on_incremental(add(2, 1, Side::sell, 105, 3, 1, 4));  // order ids are per ticker
    EXPECT_EQ(builder.status(), BookBuilder::Status::live);
    EXPECT_EQ(builder.last_seq(), 2u);
    EXPECT_EQ(builder.book(2).find(1)->side, Side::buy);
    EXPECT_EQ(builder.book(4).find(1)->side, Side::sell);
    EXPECT_EQ(builder.updates_received(), 2u);
}

TEST(BookBuilder, KeepsEachOrdersHistoryAndEachTickersLastTrade) {
    BookBuilder builder;
    builder.on_incremental(add(1, 7, Side::sell, 100, 10, 1));
    builder.on_incremental(trade(2, Side::buy, 100, 4));
    builder.on_incremental(modify(3, 7, Side::sell, 100, 6));
    builder.on_incremental(cancel(4, 7, Side::sell, 100));
    const auto history = builder.history(0, 7);
    ASSERT_EQ(history.size(), 3u);
    EXPECT_EQ(history[0], (OrderEvent{.seq = 1, .type = Type::add, .side = Side::sell, .price = 100, .qty = 10}));
    EXPECT_EQ(history[1].qty, 6u);
    EXPECT_EQ(history[2].type, Type::cancel);
    EXPECT_TRUE(builder.history(0, 8).empty());
    ASSERT_TRUE(builder.last_trade(0));
    EXPECT_EQ(builder.last_trade(0)->qty, 4u);
}

TEST(BookBuilder, AGapStartsRecoveryAndASnapshotEndsIt) {
    BookBuilder builder;
    builder.on_incremental(add(1, 1, Side::buy, 100, 5, 1));
    builder.on_incremental(add(3, 3, Side::buy, 99, 5, 1));  // 2 is missing
    EXPECT_EQ(builder.status(), BookBuilder::Status::recovering);
    ASSERT_TRUE(builder.recovering_because());
    EXPECT_NE(builder.recovering_because()->find("update 3 arrived when 2 was next"), std::string::npos);
    EXPECT_EQ(builder.last_seq(), 1u) << "the books are as of the last update applied";
    builder.on_incremental(cancel(4, 1, Side::buy, 100));

    // The exchange's snapshot as of update 2: orders 1 and 2 rest. Updates 3 and 4 follow it.
    feed_snapshot(builder, snapshot(2, {add(0, 1, Side::buy, 100, 5, 1), add(0, 2, Side::buy, 101, 1, 1)}));
    EXPECT_EQ(builder.status(), BookBuilder::Status::live);
    EXPECT_EQ(builder.last_seq(), 4u);
    EXPECT_EQ(builder.book(0).orders(), (std::vector<Order>{{2, Side::buy, 101, 1, 1}, {3, Side::buy, 99, 5, 1}}));
    ASSERT_EQ(builder.recoveries().size(), 1u);
    EXPECT_EQ(builder.recoveries()[0].after_seq, 1u);
    EXPECT_EQ(builder.recoveries()[0].snapshot_seq, 2u);
    EXPECT_FALSE(builder.recovering_because());
    const auto history = builder.history(0, 2);
    ASSERT_EQ(history.size(), 1u);
    EXPECT_TRUE(history[0].from_snapshot);
    EXPECT_EQ(history[0].seq, 2u);
}

TEST(BookBuilder, JoiningAFeedAlreadyRunningRecoversFromTheNextSnapshot) {
    BookBuilder builder;
    builder.on_incremental(add(41, 9, Side::sell, 105, 2, 1));
    EXPECT_EQ(builder.status(), BookBuilder::Status::recovering);
    EXPECT_NE(builder.recovering_because()->find("the first update to arrive was 41"), std::string::npos);
    feed_snapshot(builder, snapshot(40, {add(0, 3, Side::buy, 100, 5, 1, 6)}));
    EXPECT_EQ(builder.status(), BookBuilder::Status::live);
    EXPECT_EQ(builder.last_seq(), 41u);
    EXPECT_EQ(builder.book(6).order_count(), 1u);
    EXPECT_EQ(builder.book(0).order_count(), 1u);
}

TEST(BookBuilder, SnapshotsAreIgnoredWhileLive) {
    BookBuilder builder;
    builder.on_incremental(add(1, 1, Side::buy, 100, 5, 1));
    feed_snapshot(builder, snapshot(1, {}));
    EXPECT_EQ(builder.book(0).order_count(), 1u);
    EXPECT_TRUE(builder.recoveries().empty());
}

TEST(BookBuilder, ASnapshotThatEndsBeforeTheQueuedUpdatesBeginIsNotEnough) {
    BookBuilder builder;
    builder.on_incremental(add(50, 1, Side::buy, 100, 5, 1));
    feed_snapshot(builder, snapshot(45, {}));  // updates 46 to 49 are lost: the books can't be rebuilt
    EXPECT_EQ(builder.status(), BookBuilder::Status::recovering);
    builder.on_incremental(add(51, 2, Side::buy, 99, 5, 1));
    feed_snapshot(builder, snapshot(50, {add(0, 1, Side::buy, 100, 5, 1)}));
    EXPECT_EQ(builder.status(), BookBuilder::Status::live);
    EXPECT_EQ(builder.last_seq(), 51u);
    EXPECT_EQ(builder.book(0).order_count(), 2u);
}

TEST(BookBuilder, AnIncompleteSnapshotIsNotUsed) {
    BookBuilder builder;
    builder.on_incremental(add(5, 1, Side::buy, 100, 5, 1));
    auto updates = snapshot(5, {add(0, 1, Side::buy, 100, 5, 1)});
    updates.erase(updates.begin() + 3);  // a datagram lost from the middle
    feed_snapshot(builder, updates);
    EXPECT_EQ(builder.status(), BookBuilder::Status::recovering);
    updates = snapshot(5, {add(0, 1, Side::buy, 100, 5, 1)});
    feed_snapshot(builder, std::vector(updates.begin() + 1, updates.end()));  // its start lost
    EXPECT_EQ(builder.status(), BookBuilder::Status::recovering);
    feed_snapshot(builder, snapshot(5, {add(0, 1, Side::buy, 100, 5, 1)}));  // whole
    EXPECT_EQ(builder.status(), BookBuilder::Status::live);
}

TEST(BookBuilder, AnUpdateTheBookCantFollowStartsRecovery) {
    BookBuilder builder;
    builder.on_incremental(add(1, 1, Side::buy, 100, 5, 1));
    builder.on_incremental(cancel(2, 9, Side::buy, 100));  // an order never seen
    EXPECT_EQ(builder.status(), BookBuilder::Status::recovering);
    EXPECT_NE(builder.recovering_because()->find("isn't in the book"), std::string::npos);
    EXPECT_EQ(builder.last_seq(), 1u);
    feed_snapshot(builder, snapshot(2, {}));
    EXPECT_EQ(builder.status(), BookBuilder::Status::live);
    EXPECT_EQ(builder.book(0).order_count(), 0u);
}

TEST(BookBuilder, ATickerTheExchangeDoesntHaveStartsRecovery) {
    BookBuilder builder;
    builder.on_incremental(add(1, 1, Side::buy, 100, 5, 1, BookBuilder::kTickers));
    EXPECT_EQ(builder.status(), BookBuilder::Status::recovering);
    EXPECT_NE(builder.recovering_because()->find("ticker 8"), std::string::npos);
}

TEST(BookBuilder, ASnapshotRebuildsEveryBookAndForgetsItsTrades) {
    BookBuilder builder;
    builder.on_incremental(add(1, 1, Side::buy, 100, 5, 1, 3));
    builder.on_incremental(trade(2, Side::sell, 100, 1, 3));
    builder.on_incremental(add(4, 2, Side::buy, 99, 5, 1, 3));  // 3 missing
    feed_snapshot(builder, snapshot(3, {add(0, 5, Side::sell, 110, 1, 1, 3)}));
    EXPECT_EQ(builder.book(3).orders(), (std::vector<Order>{{2, Side::buy, 99, 5, 1}, {5, Side::sell, 110, 1, 1}}));
    EXPECT_FALSE(builder.last_trade(3));
}

// ---- A replay's market data file ----

// The file as exchange_replay writes it: magic, the recording's path, then the updates' bytes.
std::string market_data_bytes(const std::string& recording, const std::vector<MarketUpdate>& updates) {
    std::string bytes = "LLMDAT01";
    const auto length = static_cast<std::uint32_t>(recording.size());
    bytes.append(reinterpret_cast<const char*>(&length), sizeof length);
    bytes += recording;
    for (const MarketUpdate& update : updates) {
        const auto encoded = encode_update(update);
        bytes.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    }
    return bytes;
}

class MarketDataFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               std::format("fillscope-md-{}-{}", ::getpid(), ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::filesystem::path write(const std::string& bytes) const {
        const auto path = dir_ / "market.dat";
        std::ofstream{path, std::ios::binary} << bytes;
        return path;
    }
    std::filesystem::path dir_;
};

TEST_F(MarketDataFileTest, ReadsTheRecordingAndTheUpdates) {
    const std::vector<MarketUpdate> updates{add(1, 7, Side::buy, 100, 5, 1), cancel(2, 7, Side::buy, 100)};
    const auto file = read_market_data_file(write(market_data_bytes("runs/a.rec", updates)));
    ASSERT_TRUE(file) << file.error();
    EXPECT_EQ(file->recording, "runs/a.rec");
    EXPECT_EQ(file->updates, updates);
}

TEST_F(MarketDataFileTest, SaysWhyAFileCantBeRead) {
    EXPECT_NE(read_market_data_file(dir_ / "none.dat").error().find("can't open"), std::string::npos);
    EXPECT_NE(read_market_data_file(write("LLREQ001")).error().find("isn't a replay's market data"), std::string::npos);
    EXPECT_NE(read_market_data_file(write("LLMDAT01\x05")).error().find("ends inside its header"), std::string::npos);
    EXPECT_NE(read_market_data_file(write(market_data_bytes("a.rec", {}).substr(0, 14))).error().find("ends inside its header"),
              std::string::npos);
    const std::string whole = market_data_bytes("a.rec", {add(1, 7, Side::buy, 100, 5, 1)});
    EXPECT_NE(read_market_data_file(write(whole.substr(0, whole.size() - 1))).error().find("isn't a whole number"),
              std::string::npos);
    EXPECT_NE(read_market_data_file(write(market_data_bytes("a.rec", {add(2, 7, Side::buy, 100, 5, 1)}))).error().find(
                  "update 1 is numbered 2"),
              std::string::npos);
    EXPECT_TRUE(read_market_data_file(write(market_data_bytes("a.rec", {}))));  // a run with no updates is still a run
}

// ---- Receiving ----

void send_to(const Stream& stream, std::span<const std::byte> bytes) {
    EXPECT_TRUE(test::send_multicast(stream.group, stream.port, bytes.data(), bytes.size()));
}

// Receives until `count` updates have arrived or a second has passed.
std::vector<MarketUpdate> receive(FeedSocket& socket, std::size_t count, std::optional<std::string>* error = nullptr) {
    std::vector<MarketUpdate> updates;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (updates.size() < count && std::chrono::steady_clock::now() < deadline) {
        const auto received = socket.receive(updates, 16);
        if (error && received.error) {
            *error = received.error;
        }
        if (received.datagrams == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    return updates;
}

// Test-only groups and ports, one per test (tests run in parallel), none the exchange's.
TEST(FeedSocket, ReceivesWhatIsSentToItsGroup) {
    const Stream stream{"233.252.16.1", 22001};
    auto socket = FeedSocket::join(stream);
    ASSERT_TRUE(socket) << socket.error().message();
    const MarketUpdate update = add(1, 1, Side::buy, 100, 5, 1);
    send_to(stream, encode_update(update));
    const auto received = receive(*socket, 1);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], update);
}

TEST(FeedSocket, IgnoresAnotherGroupOnTheSamePort) {
    const Stream mine{"233.252.16.2", 22002}, other{"233.252.16.3", 22002};
    auto socket = FeedSocket::join(mine);
    ASSERT_TRUE(socket) << socket.error().message();
    auto other_socket = FeedSocket::join(other);  // joined on this machine, so the kernel delivers it
    ASSERT_TRUE(other_socket) << other_socket.error().message();
    send_to(other, encode_update(add(1, 1, Side::buy, 100, 5, 1)));
    send_to(mine, encode_update(add(2, 2, Side::buy, 100, 5, 2)));
    const auto received = receive(*socket, 2);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].seq, 2u);
}

TEST(FeedSocket, SaysWhyADatagramCouldntBeDecodedAndGoesOn) {
    const Stream stream{"233.252.16.4", 22004};
    auto socket = FeedSocket::join(stream);
    ASSERT_TRUE(socket) << socket.error().message();
    const auto bytes = encode_update(add(1, 1, Side::buy, 100, 5, 1));
    send_to(stream, std::span{bytes}.first(10));
    send_to(stream, bytes);
    std::optional<std::string> error;
    const auto received = receive(*socket, 1, &error);
    EXPECT_EQ(received.size(), 1u);
    ASSERT_TRUE(error);
    EXPECT_NE(error->find("10 bytes"), std::string::npos);
}

TEST(FeedSocket, RefusesAGroupThatIsntAnAddress) {
    EXPECT_FALSE(FeedSocket::join(Stream{"not an address", 22005}));
}

}  // namespace
}  // namespace fillscope
