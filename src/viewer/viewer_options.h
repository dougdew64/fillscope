// The viewer's command line.
#pragma once

#include "channel/net.h"

#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace fillscope {

inline constexpr std::string_view kDefaultContextFile = ".fillscope/context.json";

struct ViewerOptions {
    double exit_after_seconds = 0;  // 0: run until the window is closed
    std::string socket_name{kDefaultSocketName};
    // Relative paths are relative to the working directory: the repository, when started from VS Code.
    std::filesystem::path context_file{kDefaultContextFile};
};

// The options a command line asks for, or why it can't be read. `args` excludes the program's name.
[[nodiscard]] std::expected<ViewerOptions, std::string> parse_viewer_args(std::span<const char* const> args);

inline constexpr std::string_view kViewerUsage =
    "usage: viewer [--exit-after SECONDS] [--socket NAME] [--context-file PATH]";

}  // namespace fillscope
