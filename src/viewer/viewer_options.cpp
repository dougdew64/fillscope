#include "viewer/viewer_options.h"

#include "common/parse.h"

#include <format>
#include <string_view>

namespace fillscope {

std::expected<ViewerOptions, std::string> parse_viewer_args(std::span<const char* const> args) {
    ViewerOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view option{args[i]};
        if (option != "--exit-after" && option != "--socket" && option != "--context-file") {
            return std::unexpected{std::format("unknown option '{}'", option)};
        }
        if (i + 1 == args.size()) {
            return std::unexpected{std::format("{} needs a value", option)};
        }
        const std::string_view value{args[++i]};
        if (option == "--socket") {
            options.socket_name = value;
        } else if (option == "--context-file") {
            if (value.empty()) {
                return std::unexpected{"--context-file needs a path"};
            }
            options.context_file = value;
        } else {
            const auto seconds = parse_number<double>(value);
            if (!seconds) {
                return std::unexpected{std::format("--exit-after: {}", seconds.error())};
            }
            // `!(x >= 0)` rather than `x < 0`, so a NaN (which compares false with everything) is
            // refused too.
            if (!(*seconds >= 0)) {
                return std::unexpected{std::format("--exit-after: '{}' is not a number of seconds", value)};
            }
            options.exit_after_seconds = *seconds;
        }
    }
    return options;
}

}  // namespace fillscope
