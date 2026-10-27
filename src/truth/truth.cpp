#include "truth/truth.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <tuple>

namespace fillscope {

static_assert(std::endian::native == std::endian::little, "the back end's files are little-endian");

namespace {

constexpr std::string_view kMagic = "LLTRUE01";
constexpr std::size_t kOrderSize = 4 + 8 + 4;
constexpr std::size_t kTradeSize = 8 + 4 + 8 + 4 + 8;

auto order_key(const OrderTruth& order) { return std::tuple{order.ticker, order.order_id}; }

// Reads the fields of a byte string in order, each copied (the file is packed, so unaligned).
class Reader {
public:
    explicit Reader(const std::string& bytes, std::size_t at) : bytes_{bytes}, at_{at} {}

    [[nodiscard]] std::size_t left() const noexcept { return bytes_.size() - at_; }

    template <typename T>
    T take() noexcept {
        T value;
        std::memcpy(&value, bytes_.data() + at_, sizeof value);
        at_ += sizeof value;
        return value;
    }

private:
    const std::string& bytes_;
    std::size_t at_;
};

template <typename T>
void put(std::vector<std::byte>& bytes, T value) {
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof value);
}

}  // namespace

Truth::Truth(std::string recording, std::vector<OrderTruth> orders, std::vector<TradeTruth> trades)
    : recording_{std::move(recording)}, orders_{std::move(orders)}, by_id_{orders_}, trades_{std::move(trades)} {
    std::ranges::sort(by_id_, {}, order_key);
}

std::optional<std::uint32_t> Truth::client_of(std::uint32_t ticker, std::uint64_t order_id) const {
    const auto found = std::ranges::lower_bound(by_id_, std::tuple{ticker, order_id}, {}, order_key);
    if (found == by_id_.end() || order_key(*found) != std::tuple{ticker, order_id}) {
        return std::nullopt;
    }
    return found->client;
}

const TradeTruth* Truth::trade_at(std::uint64_t update) const {
    const auto found = std::ranges::lower_bound(trades_, update, {}, &TradeTruth::update);
    return found != trades_.end() && found->update == update ? &*found : nullptr;
}

std::expected<Truth, std::string> read_truth_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected{std::format("can't open {}", path.string())};
    }
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (!bytes.starts_with(kMagic)) {
        return std::unexpected{std::format("{} isn't a replay's truth (it doesn't start with {})", path.string(), kMagic)};
    }
    const auto cut_short = [&] { return std::unexpected{std::format("{} is cut short", path.string())}; };
    Reader reader{bytes, kMagic.size()};
    if (reader.left() < 4) {
        return cut_short();
    }
    const auto length = reader.take<std::uint32_t>();
    if (reader.left() < length) {
        return cut_short();
    }
    std::string recording = bytes.substr(kMagic.size() + 4, length);
    Reader body{bytes, kMagic.size() + 4 + length};
    if (body.left() < 8) {
        return cut_short();
    }
    const auto order_count = body.take<std::uint64_t>();
    if (body.left() / kOrderSize < order_count) {
        return cut_short();
    }
    std::vector<OrderTruth> orders(order_count);
    for (OrderTruth& order : orders) {
        order.ticker = body.take<std::uint32_t>();
        order.order_id = body.take<std::uint64_t>();
        order.client = body.take<std::uint32_t>();
    }
    if (body.left() < 8) {
        return cut_short();
    }
    const auto trade_count = body.take<std::uint64_t>();
    if (body.left() / kTradeSize < trade_count) {
        return cut_short();
    }
    std::vector<TradeTruth> trades(trade_count);
    for (TradeTruth& trade : trades) {
        trade.update = body.take<std::uint64_t>();
        trade.incoming_client = body.take<std::uint32_t>();
        trade.incoming_order_id = body.take<std::uint64_t>();
        trade.resting_client = body.take<std::uint32_t>();
        trade.resting_order_id = body.take<std::uint64_t>();
    }
    if (body.left() != 0) {
        return std::unexpected{std::format("{} has {} bytes after its trades", path.string(), body.left())};
    }
    for (std::size_t i = 1; i < trades.size(); ++i) {
        if (trades[i].update <= trades[i - 1].update) {
            return std::unexpected{std::format("{}: the trade at update {} comes after the one at {}", path.string(),
                                               trades[i].update, trades[i - 1].update)};
        }
    }
    Truth truth{std::move(recording), std::move(orders), std::move(trades)};
    std::vector<OrderTruth> sorted = truth.orders();
    std::ranges::sort(sorted, {}, order_key);
    if (const auto twice = std::ranges::adjacent_find(sorted, {}, order_key); twice != sorted.end()) {
        return std::unexpected{std::format("{}: order {} on ticker {} is named twice", path.string(), twice->order_id, twice->ticker)};
    }
    return truth;
}

std::vector<std::byte> encode_truth_file(const Truth& truth) {
    std::vector<std::byte> bytes;
    for (const char c : kMagic) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    put(bytes, static_cast<std::uint32_t>(truth.recording().size()));
    for (const char c : truth.recording()) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    put(bytes, static_cast<std::uint64_t>(truth.orders().size()));
    for (const OrderTruth& order : truth.orders()) {
        put(bytes, order.ticker);
        put(bytes, order.order_id);
        put(bytes, order.client);
    }
    put(bytes, static_cast<std::uint64_t>(truth.trades().size()));
    for (const TradeTruth& trade : truth.trades()) {
        put(bytes, trade.update);
        put(bytes, trade.incoming_client);
        put(bytes, trade.incoming_order_id);
        put(bytes, trade.resting_client);
        put(bytes, trade.resting_order_id);
    }
    return bytes;
}

}  // namespace fillscope
