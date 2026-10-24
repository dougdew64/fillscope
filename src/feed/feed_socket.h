// A listener on one of the exchange's multicast streams.
#pragma once

#include "common/fd.h"
#include "feed/market_update.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fillscope {

// Where a stream is sent. The exchange's are fixed in its code (exchange_main.cpp there), and it sends
// them on the loopback interface, so they never leave this machine.
struct Stream {
    std::string group;
    std::uint16_t port = 0;

    friend bool operator==(const Stream&, const Stream&) = default;
};

inline const Stream kExchangeIncremental{"233.252.14.3", 20001};
inline const Stream kExchangeSnapshot{"233.252.14.1", 20000};

class FeedSocket {
public:
    // A member of the stream's group on the loopback interface, as the trading clients join, so it
    // receives what the exchange sends there (and nothing sent to the group on another interface). Bound to the group's address and port, so it receives only that
    // group's datagrams; other programs (the trading clients) can listen too. Non-blocking.
    [[nodiscard]] static std::expected<FeedSocket, std::error_code> join(const Stream& stream);

    struct Received {
        std::size_t datagrams = 0;
        std::optional<std::string> error;  // a datagram that wasn't whole updates, or a failed receive
    };

    // Decodes the datagrams waiting, up to `max_datagrams`, into `out` (appended), without waiting for
    // more. A datagram that can't be decoded is dropped, and the error says why.
    Received receive(std::vector<MarketUpdate>& out, std::size_t max_datagrams);

private:
    explicit FeedSocket(Fd fd) : fd_{std::move(fd)} {}

    Fd fd_;
    std::vector<std::byte> buffer_ = std::vector<std::byte>(64 * 1024);
};

}  // namespace fillscope
