// The books as text (fillscope-books, src/feed/books_main.cpp): each ticker's book as the viewer
// lists it, asks above bids, each side's best price nearest the middle, each price's orders in the
// order they will fill; and its last trade.
#pragma once

#include "feed/time_shift.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fillscope {

// The books of `moment`: one ticker's if `ticker` is given, otherwise every ticker that has orders
// or a last trade, and a line saying the others are empty.
[[nodiscard]] std::string books_text(const Moment& moment, std::optional<std::uint32_t> ticker);

}  // namespace fillscope
