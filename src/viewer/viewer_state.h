// What the viewer has received: the exchange's books, kept in step with its market data
// (feed/book_builder.h), and how the market data is arriving.
#pragma once

#include "feed/book_builder.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fillscope {

struct ViewerState {
    BookBuilder books;
    // The ticker whose book is shown: the user's choice of view, which the context publishes as
    // background (the assistant should know what the user is looking at).
    std::uint32_t shown_ticker = 0;
    // Why the last datagram couldn't be received or decoded, if one couldn't.
    std::optional<std::string> feed_error;
};

}  // namespace fillscope
