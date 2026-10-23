#include "feed/book.h"

#include <algorithm>
#include <format>

namespace fillscope {

std::expected<void, std::string> Book::apply(const MarketUpdate& update) {
    const auto unexpected = [&](std::string_view why) {
        return std::unexpected{std::format("update {} ({} of order {}): {}", update.seq, to_string(update.type),
                                           update.order_id, why)};
    };
    switch (update.type) {
    case UpdateType::add: {
        if (update.side != Side::buy && update.side != Side::sell) {
            return unexpected("an order needs a side");
        }
        if (orders_.contains(update.order_id)) {
            return unexpected("the order is already in the book");
        }
        // Checked before anything changes, so a rejected add leaves the book as it was.
        const auto taken = [&](const auto& levels) -> std::optional<std::uint64_t> {
            const auto level = levels.find(update.price);
            if (level == levels.end() || !level->second.contains(update.priority)) {
                return std::nullopt;
            }
            return level->second.at(update.priority);
        };
        if (const auto holder = update.side == Side::buy ? taken(bids_) : taken(asks_)) {
            return unexpected(std::format("order {} already has priority {} at {}", *holder, update.priority, update.price));
        }
        if (update.side == Side::buy) {
            bids_[update.price].emplace(update.priority, update.order_id);
        } else {
            asks_[update.price].emplace(update.priority, update.order_id);
        }
        orders_.emplace(update.order_id, Order{update.order_id, update.side, update.price, update.qty, update.priority});
        return {};
    }
    case UpdateType::modify: {
        const auto found = orders_.find(update.order_id);
        if (found == orders_.end()) {
            return unexpected("the order isn't in the book");
        }
        if (found->second.side != update.side || found->second.price != update.price) {
            return unexpected(std::format("the book has it as a {} at {}", to_string(found->second.side), found->second.price));
        }
        found->second.qty = update.qty;
        return {};
    }
    case UpdateType::cancel: {
        const auto found = orders_.find(update.order_id);
        if (found == orders_.end()) {
            return unexpected("the order isn't in the book");
        }
        const Order& order = found->second;
        if (order.side != update.side || order.price != update.price) {
            return unexpected(std::format("the book has it as a {} at {}", to_string(order.side), order.price));
        }
        const auto remove = [&](auto& levels) {
            const auto level = levels.find(order.price);
            level->second.erase(order.priority);
            if (level->second.empty()) {
                levels.erase(level);
            }
        };
        if (order.side == Side::buy) {
            remove(bids_);
        } else {
            remove(asks_);
        }
        orders_.erase(found);
        return {};
    }
    case UpdateType::trade:
        return {};
    case UpdateType::clear:
        clear();
        return {};
    case UpdateType::snapshot_start:
    case UpdateType::snapshot_end:
        break;
    }
    return unexpected("not an update a book applies");
}

void Book::clear() {
    orders_.clear();
    bids_.clear();
    asks_.clear();
}

const Order* Book::find(std::uint64_t order_id) const {
    const auto found = orders_.find(order_id);
    return found == orders_.end() ? nullptr : &found->second;
}

Level Book::make_level(Side side, std::int64_t price, const Queue& queue) const {
    Level level{.side = side, .price = price, .qty = 0, .orders = {}};
    level.orders.reserve(queue.size());
    for (const auto& [priority, order_id] : queue) {
        const Order& order = orders_.at(order_id);
        level.qty += order.qty;
        level.orders.push_back(order);
    }
    return level;
}

std::vector<Level> Book::levels(Side side) const {
    std::vector<Level> result;
    const auto collect = [&](const auto& levels) {
        result.reserve(levels.size());
        for (const auto& [price, queue] : levels) {
            result.push_back(make_level(side, price, queue));
        }
    };
    if (side == Side::buy) {
        collect(bids_);
    } else if (side == Side::sell) {
        collect(asks_);
    }
    return result;
}

std::optional<Level> Book::level(Side side, std::int64_t price) const {
    const auto find_in = [&](const auto& levels) -> std::optional<Level> {
        const auto found = levels.find(price);
        if (found == levels.end()) {
            return std::nullopt;
        }
        return make_level(side, price, found->second);
    };
    if (side == Side::buy) {
        return find_in(bids_);
    }
    if (side == Side::sell) {
        return find_in(asks_);
    }
    return std::nullopt;
}

std::vector<Order> Book::orders() const {
    std::vector<Order> result;
    result.reserve(orders_.size());
    for (const auto& [id, order] : orders_) {
        result.push_back(order);
    }
    std::ranges::sort(result, {}, &Order::order_id);
    return result;
}

}  // namespace fillscope
