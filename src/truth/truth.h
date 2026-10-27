// A replay's truth, as the back end's exchange_replay --truth writes it (exchange/matcher/replay.h
// there): which client sent each order, and the client on each side of each trade. The market data
// never says who sent what, and the rest of Fillscope never reads a client from it; the truth is
// read only to show what really happened in a replay, and to score what is detected against it.
//
// The file: 8 bytes, "LLTRUE01"; the recording's path, as a 4-byte length and its bytes; the number
// of orders (8 bytes), then each order the matcher accepted, in the order accepted: its ticker (4),
// its market order id (8; numbered from 1 on each ticker, as in the market data), and its client
// (4); then the number of trades (8), then each trade, in the market data's order: its update's
// number (8), the incoming order's client (4) and market order id (8), then the resting order's
// (4 and 8). Little-endian and packed. Read here from its layout, not the back end's headers, as
// the other files are (answer/wire.h); the back end's tests pin it down.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fillscope {

struct OrderTruth {
    std::uint32_t ticker = 0;
    std::uint64_t order_id = 0;  // the market order id, as in the market data
    std::uint32_t client = 0;

    friend bool operator==(const OrderTruth&, const OrderTruth&) = default;
};

// A trade: its update in the market data, the order that came in and took liquidity, and the one
// resting on the book that it met.
struct TradeTruth {
    std::uint64_t update = 0;
    std::uint32_t incoming_client = 0;
    std::uint64_t incoming_order_id = 0;
    std::uint32_t resting_client = 0;
    std::uint64_t resting_order_id = 0;

    friend bool operator==(const TradeTruth&, const TradeTruth&) = default;
};

class Truth {
public:
    // `orders` must name each (ticker, order id) once, and `trades` be in increasing update order;
    // read_truth_file checks both.
    Truth(std::string recording, std::vector<OrderTruth> orders, std::vector<TradeTruth> trades);

    // The recording it was replayed from, as exchange_replay was given it.
    [[nodiscard]] const std::string& recording() const noexcept { return recording_; }
    // Every order, in the order the matcher accepted them.
    [[nodiscard]] const std::vector<OrderTruth>& orders() const noexcept { return orders_; }
    // Every trade, in the market data's order.
    [[nodiscard]] const std::vector<TradeTruth>& trades() const noexcept { return trades_; }

    // The client that sent the order, or nothing if no order has that id on that ticker.
    [[nodiscard]] std::optional<std::uint32_t> client_of(std::uint32_t ticker, std::uint64_t order_id) const;
    // The trade at that update, or nothing if it isn't a trade's.
    [[nodiscard]] const TradeTruth* trade_at(std::uint64_t update) const;

private:
    std::string recording_;
    std::vector<OrderTruth> orders_;
    std::vector<OrderTruth> by_id_;  // the orders, sorted by ticker and id, to look one up
    std::vector<TradeTruth> trades_;
};

// The file's truth, or why it can't be read: no file, not a truth file, cut short or with bytes
// after its trades, an order named twice, or trades out of order.
[[nodiscard]] std::expected<Truth, std::string> read_truth_file(const std::filesystem::path& path);

// A truth file's bytes, as the replay writes them, for tests.
[[nodiscard]] std::vector<std::byte> encode_truth_file(const Truth& truth);

}  // namespace fillscope
