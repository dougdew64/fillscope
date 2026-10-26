#include "feed/books_text.h"

#include <format>
#include <vector>

namespace fillscope {
namespace {

void one_book(std::string& out, const Moment& moment, std::uint32_t ticker) {
    const Book& book = moment.books[ticker];
    out += std::format("ticker {}: {}\n", ticker, book.order_count() == 1 ? std::string{"1 order"} : std::format("{} orders", book.order_count()));
    const std::vector<Level> asks = book.levels(Side::sell);
    const auto row = [&](const Level& level) {
        for (std::size_t place = 0; place < level.orders.size(); ++place) {
            const Order& order = level.orders[place];
            out += std::format("  {:<4} {:>8}  qty {:>6}  order {:>8}  #{} at its price{}\n", to_string(level.side), level.price,
                               order.qty, order.order_id, place + 1,
                               place == 0 ? std::format(" (level qty {})", level.qty) : std::string{});
        }
    };
    for (auto level = asks.rbegin(); level != asks.rend(); ++level) {  // worst ask at the top
        row(*level);
    }
    for (const Level& level : book.levels(Side::buy)) {  // best bid at the top of the bids
        row(level);
    }
    if (const auto& trade = moment.last_trades[ticker]) {
        out += std::format("  last trade: {} {} at {}, update {}\n", trade->qty, trade->side == Side::buy ? "bought" : "sold",
                           trade->price, trade->seq);
    }
}

}  // namespace

std::string books_text(const Moment& moment, std::optional<std::uint32_t> ticker) {
    std::string out;
    if (ticker) {
        one_book(out, moment, *ticker);
        return out;
    }
    std::vector<std::uint32_t> empty;
    for (std::uint32_t each = 0; each < BookBuilder::kTickers; ++each) {
        if (moment.books[each].order_count() == 0 && !moment.last_trades[each]) {
            empty.push_back(each);
        } else {
            one_book(out, moment, each);
        }
    }
    if (!empty.empty()) {
        std::string list;
        for (std::size_t i = 0; i < empty.size(); ++i) {
            list += std::format("{}{}", i == 0 ? "" : ", ", empty[i]);
        }
        out += std::format("ticker{} {}: no orders, no trade\n", empty.size() == 1 ? "" : "s", list);
    }
    return out;
}

}  // namespace fillscope
