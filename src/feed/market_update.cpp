#include "feed/market_update.h"

#include <bit>
#include <cstring>
#include <format>

namespace fillscope {

// The wire is little-endian, and the fields are copied as they are.
static_assert(std::endian::native == std::endian::little, "decoding assumes a little-endian machine");

namespace {

// Where each field starts in an update's 42 bytes.
constexpr std::size_t kSeqAt = 0, kTypeAt = 8, kOrderIdAt = 9, kTickerAt = 17, kSideAt = 21, kPriceAt = 22,
                      kQtyAt = 30, kPriorityAt = 34;
static_assert(kPriorityAt + sizeof(std::uint64_t) == kUpdateSize);

template <typename T>
T read(const std::byte* bytes, std::size_t at) noexcept {
    T value;
    std::memcpy(&value, bytes + at, sizeof value);  // the fields are unaligned: copied, never cast
    return value;
}

template <typename T>
void write(std::byte* bytes, std::size_t at, T value) noexcept {
    std::memcpy(bytes + at, &value, sizeof value);
}

}  // namespace

std::string_view to_string(UpdateType type) noexcept {
    switch (type) {
    case UpdateType::clear: return "clear";
    case UpdateType::add: return "add";
    case UpdateType::modify: return "modify";
    case UpdateType::cancel: return "cancel";
    case UpdateType::trade: return "trade";
    case UpdateType::snapshot_start: return "snapshot start";
    case UpdateType::snapshot_end: return "snapshot end";
    }
    return "unknown";
}

std::string_view to_string(Side side) noexcept {
    switch (side) {
    case Side::buy: return "buy";
    case Side::sell: return "sell";
    case Side::none: return "none";
    }
    return "unknown";
}

std::expected<std::vector<MarketUpdate>, std::string> decode_updates(std::span<const std::byte> datagram) {
    if (datagram.size() % kUpdateSize != 0) {
        return std::unexpected{std::format("a datagram of {} bytes isn't a whole number of {}-byte updates",
                                           datagram.size(), kUpdateSize)};
    }
    std::vector<MarketUpdate> updates;
    updates.reserve(datagram.size() / kUpdateSize);
    for (std::size_t offset = 0; offset < datagram.size(); offset += kUpdateSize) {
        const std::byte* bytes = datagram.data() + offset;
        const auto type = read<std::uint8_t>(bytes, kTypeAt);
        if (type < static_cast<std::uint8_t>(UpdateType::clear) || type > static_cast<std::uint8_t>(UpdateType::snapshot_end)) {
            return std::unexpected{std::format("update {} of the datagram has type {}, which isn't one", offset / kUpdateSize, type)};
        }
        const auto side = read<std::int8_t>(bytes, kSideAt);
        if (side != 0 && side != 1 && side != -1) {
            return std::unexpected{std::format("update {} of the datagram has side {}, which isn't one", offset / kUpdateSize, side)};
        }
        updates.push_back(MarketUpdate{.seq = read<std::uint64_t>(bytes, kSeqAt),
                                       .type = static_cast<UpdateType>(type),
                                       .order_id = read<std::uint64_t>(bytes, kOrderIdAt),
                                       .ticker = read<std::uint32_t>(bytes, kTickerAt),
                                       .side = static_cast<Side>(side),
                                       .price = read<std::int64_t>(bytes, kPriceAt),
                                       .qty = read<std::uint32_t>(bytes, kQtyAt),
                                       .priority = read<std::uint64_t>(bytes, kPriorityAt)});
    }
    return updates;
}

std::array<std::byte, kUpdateSize> encode_update(const MarketUpdate& update) noexcept {
    std::array<std::byte, kUpdateSize> bytes{};
    write(bytes.data(), kSeqAt, update.seq);
    write(bytes.data(), kTypeAt, static_cast<std::uint8_t>(update.type));
    write(bytes.data(), kOrderIdAt, update.order_id);
    write(bytes.data(), kTickerAt, update.ticker);
    write(bytes.data(), kSideAt, static_cast<std::int8_t>(update.side));
    write(bytes.data(), kPriceAt, update.price);
    write(bytes.data(), kQtyAt, update.qty);
    write(bytes.data(), kPriorityAt, update.priority);
    return bytes;
}

}  // namespace fillscope
