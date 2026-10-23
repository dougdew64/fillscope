// One ticker's order book as the market data describes it: every resting order, by the exchange's
// order id, and each price's queue of orders in the order they will fill.
#pragma once

#include "feed/market_update.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fillscope {

// A resting order.
struct Order {
    std::uint64_t order_id = 0;
    Side side = Side::none;
    std::int64_t price = 0;
    std::uint32_t qty = 0;
    std::uint64_t priority = 0;  // its place in its price's queue: lower fills first

    friend bool operator==(const Order&, const Order&) = default;
};

// One price on one side: its orders in the order they will fill, and their total quantity.
struct Level {
    Side side = Side::none;
    std::int64_t price = 0;
    std::uint64_t qty = 0;
    std::vector<Order> orders;

    friend bool operator==(const Level&, const Level&) = default;
};

class Book {
public:
    // Applies an add, modify or cancel; a trade changes nothing here (the updates around it do). The
    // updates must be consistent with the book: an add for an order it has, or a modify or cancel for
    // one it hasn't, means updates were missed, and then the book is left as it was and the reason is
    // returned. A modify changes only the quantity: the exchange keeps an order's side, price and
    // place in the queue when it is partly filled.
    [[nodiscard]] std::expected<void, std::string> apply(const MarketUpdate& update);

    void clear();

    [[nodiscard]] const Order* find(std::uint64_t order_id) const;

    // The side's levels, best price first: highest bid, lowest ask.
    [[nodiscard]] std::vector<Level> levels(Side side) const;
    [[nodiscard]] std::optional<Level> level(Side side, std::int64_t price) const;

    // Every resting order, by order id: to compare two books.
    [[nodiscard]] std::vector<Order> orders() const;
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

private:
    // Each price's queue: priority to order id.
    using Queue = std::map<std::uint64_t, std::uint64_t>;
    [[nodiscard]] Level make_level(Side side, std::int64_t price, const Queue& queue) const;

    std::unordered_map<std::uint64_t, Order> orders_;
    std::map<std::int64_t, Queue, std::greater<>> bids_;  // best (highest) first
    std::map<std::int64_t, Queue> asks_;                  // best (lowest) first
};

}  // namespace fillscope
