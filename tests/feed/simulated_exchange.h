// A simulated exchange's market data, for tests that need a long, realistic run: the tests of what
// the viewer keeps (simulated_feed.h) and of a long replay (viewer/viewer_ui_test.cpp).
#pragma once

#include "feed/book_builder.h"
#include "feed/market_update.h"

#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <vector>

namespace fillscope::test {

// A simulated exchange's market data: adds, partial fills (modify), cancels and trades on every
// ticker, numbered from 1, each consistent with the books before it; and, at any point, a snapshot of
// those books, as the exchange sends it.
class Exchange {
public:
    explicit Exchange(unsigned seed) : random_{seed} {}

    MarketUpdate next() {
        MarketUpdate update{.seq = ++seq_};
        update.ticker = pick(BookBuilder::kTickers);
        std::map<std::uint64_t, MarketUpdate>& resting = resting_[update.ticker];
        const unsigned roll = pick(10);
        // Adds while a book is thin; once it holds 25 orders, more cancels and fills than adds, so it
        // stays the size of a real book.
        if (resting.size() < 5 || (resting.size() < 25 && roll < 4) || roll < 2) {
            update.type = UpdateType::add;
            update.order_id = ++order_id_;
            update.side = pick(2) == 0 ? Side::buy : Side::sell;
            update.price = 100 + static_cast<std::int64_t>(pick(10)) + (update.side == Side::sell ? 10 : 0);
            update.qty = 1 + pick(100);
            update.priority = ++priority_;
            resting[update.order_id] = update;
        } else if (roll < 6) {
            update = trade_on(update, resting);
        } else {
            auto order = resting.begin();
            std::advance(order, pick(static_cast<unsigned>(resting.size())));
            update.order_id = order->first;
            update.side = order->second.side;
            update.price = order->second.price;
            update.priority = order->second.priority;
            if (roll < 8 && order->second.qty > 1) {
                update.type = UpdateType::modify;  // partly filled: less left, its place kept
                update.qty = order->second.qty = 1 + pick(order->second.qty - 1);
            } else {
                update.type = UpdateType::cancel;
                resting.erase(order);
            }
        }
        return update;
    }

    // A whole snapshot of the books after the last update made, numbered from 0.
    std::vector<MarketUpdate> snapshot() const {
        std::vector<MarketUpdate> updates{{.type = UpdateType::snapshot_start, .order_id = seq_}};
        for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
            updates.push_back({.type = UpdateType::clear, .ticker = ticker});
            for (const auto& [id, order] : resting_[ticker]) {
                updates.push_back(order);
            }
        }
        updates.push_back({.type = UpdateType::snapshot_end, .order_id = seq_});
        for (std::uint64_t i = 0; i < updates.size(); ++i) {
            updates[i].seq = i;
        }
        return updates;
    }

private:
    unsigned pick(unsigned n) { return std::uniform_int_distribution<unsigned>{0, n - 1}(random_); }
    MarketUpdate trade_on(MarketUpdate update, const std::map<std::uint64_t, MarketUpdate>& resting) {
        const MarketUpdate& order = resting.begin()->second;
        update.type = UpdateType::trade;
        update.side = order.side == Side::buy ? Side::sell : Side::buy;
        update.price = order.price;
        update.qty = 1;
        return update;
    }

    std::mt19937 random_;
    std::uint64_t seq_ = 0;
    std::uint64_t order_id_ = 0;
    std::uint64_t priority_ = 0;
    std::array<std::map<std::uint64_t, MarketUpdate>, BookBuilder::kTickers> resting_;
};

}  // namespace fillscope::test
