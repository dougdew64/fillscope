#include "frame_source.h"

#include "common/parse.h"
#include "lockstep/anchor.h"

#include <format>
#include <optional>
#include <print>
#include <string_view>
#include <thread>

namespace fillscope {

Frame FrameGenerator::next() {
    // The order of the draws decides every frame's values for a given seed, so each is its own
    // statement, where the order is plain to see. (Inside braces, Frame{a(), b()}, C++ does
    // evaluate left to right; it is function-call arguments, f(a(), b()), whose order is
    // unspecified, and an easy refactor turns one into the other.)
    const auto step = static_cast<std::int64_t>(draw_uniform(engine_, 0, 2 * kMaxPriceStep)) - kMaxPriceStep;
    const auto qty = static_cast<std::uint32_t>(draw_uniform(engine_, kMinQty, kMaxQty));
    const auto id = static_cast<std::uint32_t>(draw_uniform(engine_, kMinId, kMaxId));
    price_ += step;
    return Frame{.index = next_index_++, .price = price_, .qty = qty, .id = id};
}

std::expected<SourceOptions, std::string> parse_source_args(std::span<const char* const> args) {
    SourceOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view option{args[i]};
        if (option == "--self-stop") {
            options.self_stop = true;
            continue;
        }
        if (option != "--frames" && option != "--interval-ms" && option != "--socket" && option != "--seed") {
            return std::unexpected{std::format("unknown option '{}'", option)};
        }
        if (i + 1 == args.size()) {
            return std::unexpected{std::format("{} needs a value", option)};
        }
        const std::string_view value{args[++i]};
        if (option == "--socket") {
            options.socket_name = value;
            continue;
        }
        // The numeric options. `auto` deduces each parse's std::expected type; `if (!number)`
        // asks whether it holds an error.
        if (option == "--interval-ms") {
            const auto milliseconds = parse_number<std::uint32_t>(value);
            if (!milliseconds) {
                return std::unexpected{std::format("--interval-ms: {}", milliseconds.error())};
            }
            options.interval = std::chrono::milliseconds{*milliseconds};
            continue;
        }
        const auto number = parse_number<std::uint64_t>(value);
        if (!number) {
            return std::unexpected{std::format("{}: {}", option, number.error())};
        }
        (option == "--frames" ? options.frames : options.seed) = *number;
    }
    return options;
}

int run_frame_source(const SourceOptions& options) {
    set_self_stop(options.self_stop);
    const auto viewer = connect_to(options.socket_name, options.connect_attempts, options.connect_retry);
    if (!viewer) {
        std::println(stderr, "frame_source: can't connect to a viewer on '{}': {}", options.socket_name,
                     viewer.error().message());
        return 1;
    }

    // Says why a send failed: the viewer going away is the usual reason, but not the only one.
    const auto report_failed_send = [](std::error_code error, std::string_view when) {
        if (receiver_gone(error)) {
            std::println(stderr, "frame_source: the viewer went away {}", when);
        } else {
            std::println(stderr, "frame_source: can't send {}: {}", when, error.message());
        }
    };

    if (const auto sent = send_message(*viewer, SourceHello{.seed = options.seed, .frame_count = options.frames}); !sent) {
        report_failed_send(sent.error(), "the run's description");
        return 1;
    }
    FrameGenerator generator{options.seed};
    std::optional<std::uint64_t> last_sent;
    for (std::uint64_t i = 0; i < options.frames; ++i) {
        const Frame frame = generator.next();
        if (const auto sent = send_message(*viewer, frame); !sent) {
            report_failed_send(sent.error(), std::format("at frame {}", frame.index));
            return 1;
        }
        last_sent = frame.index;
        fillscope_frame_anchor(frame.index);
        std::this_thread::sleep_for(options.interval);
    }
    if (last_sent) {
        std::println("frame_source: sent {} frames, the last #{}", options.frames, *last_sent);
    } else {
        std::println("frame_source: sent no frames");
    }
    return 0;
}

}  // namespace fillscope
