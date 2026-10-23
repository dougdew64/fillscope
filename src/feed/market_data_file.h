// A replay's market data, as the back end's exchange_replay writes it (exchange/matcher/replay.h
// there): the incremental updates a recording of the exchange's requests gives, numbered from 1,
// exactly as the live exchange published them for the same requests.
//
// The file: 8 bytes, "LLMDAT01"; the recording's path, as a 4-byte little-endian length and its
// bytes; then each update's 42 bytes, as on the wire (market_update.h).
#pragma once

#include "feed/market_update.h"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fillscope {

struct MarketDataFile {
    std::string recording;  // the recording it was replayed from, as exchange_replay was given it
    std::vector<MarketUpdate> updates;
};

// The file's contents, or why it can't be read: no file, not a market data file, a header cut
// short, bytes that aren't whole updates, or updates not numbered 1, 2, 3, ...
[[nodiscard]] std::expected<MarketDataFile, std::string> read_market_data_file(const std::filesystem::path& path);

}  // namespace fillscope
