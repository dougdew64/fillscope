// The viewer's command line.
#pragma once

#include "feed/feed_socket.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fillscope {

inline constexpr std::string_view kDefaultContextFile = ".fillscope/context.json";

struct ViewerOptions {
    double exit_after_seconds = 0;  // 0: run until the window is closed
    // The exchange's market data streams, by default where the exchange sends them.
    Stream incremental = kExchangeIncremental;
    Stream snapshot = kExchangeSnapshot;
    // A replay's market data file to show instead of the live exchange (exchange_replay's output).
    std::optional<std::filesystem::path> replay;
    // Relative paths are relative to the working directory: the repository, when started from VS Code.
    std::filesystem::path context_file{kDefaultContextFile};
};

// The options a command line asks for, or why it can't be read. `args` excludes the program's name.
[[nodiscard]] std::expected<ViewerOptions, std::string> parse_viewer_args(std::span<const char* const> args);

inline constexpr std::string_view kViewerUsage =
    "usage: viewer [--exit-after SECONDS] [--incremental GROUP:PORT] [--snapshot GROUP:PORT] [--replay FILE] "
    "[--context-file PATH]";

}  // namespace fillscope
