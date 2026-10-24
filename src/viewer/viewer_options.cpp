#include "viewer/viewer_options.h"

#include "common/parse.h"

#include <arpa/inet.h>

#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>

namespace fillscope {

namespace {

// A stream as GROUP:PORT: an IPv4 multicast address (224.0.0.0 to 239.255.255.255) and a port from 1.
std::expected<Stream, std::string> parse_stream(std::string_view option, std::string_view value) {
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos) {
        return std::unexpected{std::format("{}: '{}' is not GROUP:PORT", option, value)};
    }
    const std::string group{value.substr(0, colon)};
    in_addr address{};
    if (inet_pton(AF_INET, group.c_str(), &address) != 1 || (ntohl(address.s_addr) >> 28) != 0xE) {
        return std::unexpected{std::format("{}: '{}' is not a multicast address", option, group)};
    }
    const auto port = parse_number<std::uint16_t>(value.substr(colon + 1));
    if (!port || *port == 0) {
        return std::unexpected{std::format("{}: '{}' is not a port", option, value.substr(colon + 1))};
    }
    return Stream{group, *port};
}

}  // namespace

std::expected<ViewerOptions, std::string> parse_viewer_args(std::span<const char* const> args) {
    ViewerOptions options;
    bool stream_given = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view option{args[i]};
        if (option != "--exit-after" && option != "--incremental" && option != "--snapshot" && option != "--replay" &&
            option != "--context-file") {
            return std::unexpected{std::format("unknown option '{}'", option)};
        }
        if (i + 1 == args.size()) {
            return std::unexpected{std::format("{} needs a value", option)};
        }
        const std::string_view value{args[++i]};
        if (option == "--incremental" || option == "--snapshot") {
            auto stream = parse_stream(option, value);
            if (!stream) {
                return std::unexpected{std::move(stream.error())};
            }
            (option == "--incremental" ? options.incremental : options.snapshot) = std::move(*stream);
            stream_given = true;
        } else if (option == "--replay") {
            if (value.empty()) {
                return std::unexpected{"--replay needs a file"};
            }
            options.replay = value;
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
            // refused too; infinity is refused because 0 already means "never".
            if (!(*seconds >= 0) || std::isinf(*seconds)) {
                return std::unexpected{std::format("--exit-after: '{}' is not a number of seconds", value)};
            }
            options.exit_after_seconds = *seconds;
        }
    }
    if (options.replay && stream_given) {
        // A replay reads no stream, so a stream given with it would be silently ignored.
        return std::unexpected{"--replay reads a file, not the exchange's streams: give it without --incremental or --snapshot"};
    }
    return options;
}

}  // namespace fillscope
