// Reading a replay's truth (truth/truth.h): a file built byte by byte from its layout, as the back
// end's tests pin it down, read back; looked up; and every way it can be wrong said.
#include "truth/truth.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fillscope {
namespace {

class TruthFile : public ::testing::Test {
protected:
    void write(const std::string& bytes) const {
        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // The file's bytes: the header naming "a.rec", the orders' count and orders (ticker, id,
    // client), and the trades' count and trades (update, incoming client and id, resting client and
    // id).
    static std::string bytes_of(const std::vector<OrderTruth>& orders, const std::vector<TradeTruth>& trades) {
        std::string bytes = "LLTRUE01";
        const auto append = [&bytes](auto value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof value); };
        append(std::uint32_t{5});
        bytes += "a.rec";
        append(std::uint64_t{orders.size()});
        for (const OrderTruth& order : orders) {
            append(order.ticker);
            append(order.order_id);
            append(order.client);
        }
        append(std::uint64_t{trades.size()});
        for (const TradeTruth& trade : trades) {
            append(trade.update);
            append(trade.incoming_client);
            append(trade.incoming_order_id);
            append(trade.resting_client);
            append(trade.resting_order_id);
        }
        return bytes;
    }

    // The same order id on two tickers, each a different client's; two trades.
    const std::vector<OrderTruth> orders_{{.ticker = 0, .order_id = 1, .client = 1},
                                          {.ticker = 3, .order_id = 1, .client = 5},
                                          {.ticker = 0, .order_id = 2, .client = 3}};
    const std::vector<TradeTruth> trades_{
        {.update = 4, .incoming_client = 3, .incoming_order_id = 2, .resting_client = 1, .resting_order_id = 1},
        {.update = 9, .incoming_client = 1, .incoming_order_id = 7, .resting_client = 5, .resting_order_id = 1}};

    const test::TemporaryDirectory dir_{"fillscope-truth"};
    const std::filesystem::path path_ = dir_.path / "truth.dat";
};

TEST_F(TruthFile, ReadsBackEachOrderAndTradeInTheFilesOrder) {
    write(bytes_of(orders_, trades_));
    const auto truth = read_truth_file(path_);
    ASSERT_TRUE(truth) << truth.error();
    EXPECT_EQ(truth->recording(), "a.rec");
    EXPECT_EQ(truth->orders(), orders_);
    EXPECT_EQ(truth->trades(), trades_);
}

TEST_F(TruthFile, AnOrdersClientIsFoundByItsTickerAndId) {
    const Truth truth{"a.rec", orders_, trades_};
    EXPECT_EQ(truth.client_of(0, 1), 1u);
    EXPECT_EQ(truth.client_of(3, 1), 5u);
    EXPECT_EQ(truth.client_of(0, 2), 3u);
    EXPECT_EQ(truth.client_of(3, 2), std::nullopt);
    EXPECT_EQ(truth.client_of(1, 1), std::nullopt);
    EXPECT_EQ(truth.client_of(0, 3), std::nullopt);
}

TEST_F(TruthFile, ATradeIsFoundByItsUpdate) {
    const Truth truth{"a.rec", orders_, trades_};
    ASSERT_NE(truth.trade_at(4), nullptr);
    EXPECT_EQ(*truth.trade_at(4), trades_[0]);
    ASSERT_NE(truth.trade_at(9), nullptr);
    EXPECT_EQ(*truth.trade_at(9), trades_[1]);
    for (const std::uint64_t update : {0, 1, 5, 8, 10}) {
        EXPECT_EQ(truth.trade_at(update), nullptr) << "update " << update;
    }
}

TEST_F(TruthFile, ItsBytesAreWhatItReadsFrom) {
    const auto bytes = encode_truth_file(Truth{"a.rec", orders_, trades_});
    const std::string expected = bytes_of(orders_, trades_);
    ASSERT_EQ(bytes.size(), expected.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), expected);
    const auto empty = encode_truth_file(Truth{"a.rec", {}, {}});
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(empty.data()), empty.size()), bytes_of({}, {}));
}

TEST_F(TruthFile, SaysWhyItCantBeRead) {
    const auto error = [&](const std::string& bytes) {
        write(bytes);
        const auto truth = read_truth_file(path_);
        return truth ? std::string{"(read)"} : truth.error();
    };
    EXPECT_NE(read_truth_file(dir_.path / "missing.dat").error().find("can't open"), std::string::npos);
    EXPECT_NE(error("LLSTEP01").find("isn't a replay's truth"), std::string::npos);
    // Cut anywhere past the magic, it says so, never reads what isn't there.
    const std::string whole = bytes_of(orders_, trades_);
    for (std::size_t size = 8; size < whole.size(); ++size) {
        EXPECT_NE(error(whole.substr(0, size)).find("is cut short"), std::string::npos) << size << " bytes";
    }
    EXPECT_NE(error(whole + "x").find("has 1 bytes after its trades"), std::string::npos);
    // A count larger than the file could hold: cut short, not a huge allocation.
    std::string huge = bytes_of({}, {});
    huge.replace(17, 8, std::string(8, '\xff'));
    EXPECT_NE(error(huge).find("is cut short"), std::string::npos);
    EXPECT_NE(error(bytes_of({orders_[0], orders_[1], orders_[0]}, {})).find("order 1 on ticker 0 is named twice"), std::string::npos);
    EXPECT_NE(error(bytes_of(orders_, {trades_[1], trades_[0]})).find("the trade at update 4 comes after the one at 9"),
              std::string::npos);
    EXPECT_NE(error(bytes_of(orders_, {trades_[0], trades_[0]})).find("the trade at update 4 comes after the one at 4"),
              std::string::npos);
}

}  // namespace
}  // namespace fillscope
