// The exchange's market data, as Fillscope reads it: the back end's MDPMarketUpdate (backend/,
// market_data/market_update.h), decoded here from its bytes rather than by including the back
// end's header, so this decoder is a second, independent reading of the wire format, which the back
// end's own tests pin down (tests/market_update_test.cpp there).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fillscope {

// The kinds of update, with their values on the wire.
enum class UpdateType : std::uint8_t {
    clear = 1,           // in a snapshot: empty this ticker's book
    add = 2,             // an order rests in the book
    modify = 3,          // a resting order's quantity changed (it was partly filled)
    cancel = 4,          // a resting order left the book (cancelled, or filled)
    trade = 5,           // a trade, at the resting order's price; the book changes by the updates around it
    snapshot_start = 6,  // a snapshot begins; its order_id is the last incremental update it includes
    snapshot_end = 7,    // a snapshot ends; order_id as for snapshot_start
};

enum class Side : std::int8_t { none = 0, buy = 1, sell = -1 };

[[nodiscard]] std::string_view to_string(UpdateType type) noexcept;
[[nodiscard]] std::string_view to_string(Side side) noexcept;

// One update. `seq` numbers the incremental feed from 1, and a snapshot's updates from 0.
struct MarketUpdate {
    std::uint64_t seq = 0;
    UpdateType type = UpdateType::clear;
    std::uint64_t order_id = 0;  // the exchange's id, unique within a ticker
    std::uint32_t ticker = 0;
    Side side = Side::none;
    std::int64_t price = 0;
    std::uint32_t qty = 0;
    std::uint64_t priority = 0;  // an order's place in its price's queue: lower goes first

    friend bool operator==(const MarketUpdate&, const MarketUpdate&) = default;
};

// The bytes of one update on the wire: packed, little-endian.
inline constexpr std::size_t kUpdateSize = 42;

// The updates in one datagram (the exchange may put several in one), or why its bytes aren't
// whole updates of known types.
[[nodiscard]] std::expected<std::vector<MarketUpdate>, std::string> decode_updates(std::span<const std::byte> datagram);

// An update's bytes, as the exchange sends them: for tests, and for a stand-in exchange.
[[nodiscard]] std::array<std::byte, kUpdateSize> encode_update(const MarketUpdate& update) noexcept;

}  // namespace fillscope
