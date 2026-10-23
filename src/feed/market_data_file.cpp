#include "feed/market_data_file.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>

namespace fillscope {

namespace {
constexpr std::string_view kMagic = "LLMDAT01";
}

std::expected<MarketDataFile, std::string> read_market_data_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected{std::format("can't open {}", path.string())};
    }
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (!bytes.starts_with(kMagic)) {
        return std::unexpected{std::format("{} isn't a replay's market data (it doesn't start with {})", path.string(), kMagic)};
    }
    std::uint32_t length = 0;
    if (bytes.size() < kMagic.size() + sizeof length) {
        return std::unexpected{std::format("{} ends inside its header", path.string())};
    }
    std::memcpy(&length, bytes.data() + kMagic.size(), sizeof length);
    const std::size_t body = kMagic.size() + sizeof length + length;
    if (bytes.size() < body) {
        return std::unexpected{std::format("{} ends inside its header", path.string())};
    }
    MarketDataFile file{.recording = bytes.substr(kMagic.size() + sizeof length, length), .updates = {}};
    auto updates = decode_updates(std::as_bytes(std::span{bytes}.subspan(body)));
    if (!updates) {
        return std::unexpected{std::format("{}: {}", path.string(), updates.error())};
    }
    for (std::size_t i = 0; i < updates->size(); ++i) {
        if ((*updates)[i].seq != i + 1) {
            return std::unexpected{std::format("{}: update {} is numbered {}", path.string(), i + 1, (*updates)[i].seq)};
        }
    }
    file.updates = std::move(*updates);
    return file;
}

}  // namespace fillscope
