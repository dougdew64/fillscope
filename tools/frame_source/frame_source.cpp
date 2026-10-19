#include "frame_source.h"

#include "lockstep/anchor.h"

#include <cstdio>
#include <string_view>
#include <thread>

namespace fillscope {

Frame FrameGenerator::next() {
    std::uniform_int_distribution<int> step{-kMaxPriceStep, kMaxPriceStep};
    std::uniform_int_distribution<std::uint32_t> qty{kMinQty, kMaxQty};
    std::uniform_int_distribution<std::uint32_t> id{kMinId, kMaxId};
    price_ += step(rng_);
    // The order of the draws decides every frame's values for a given seed, so each is its own
    // statement, where the order is plain to see. (Inside braces, Frame{a(), b()}, C++ does
    // evaluate left to right; it is function-call arguments, f(a(), b()), whose order is
    // unspecified, and an easy refactor turns one into the other.)
    const std::uint32_t frame_qty = qty(rng_);
    const std::uint32_t frame_id = id(rng_);
    return Frame{.index = next_index_++, .price = price_, .qty = frame_qty, .id = frame_id};
}

std::optional<SourceOptions> parse_source_args(std::span<const char* const> args) {
    SourceOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        const bool has_value = i + 1 < args.size();
        if (arg == "--frames" && has_value) {
            options.frames = std::stoull(args[++i]);
        } else if (arg == "--interval-ms" && has_value) {
            options.interval = std::chrono::milliseconds{std::stoi(args[++i])};
        } else if (arg == "--socket" && has_value) {
            options.socket_name = args[++i];
        } else if (arg == "--seed" && has_value) {
            options.seed = std::stoull(args[++i]);
        } else if (arg == "--self-stop") {
            options.self_stop = true;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

int run_frame_source(const SourceOptions& options) {
    set_self_stop(options.self_stop);
    const Fd viewer = connect_to(options.socket_name, options.connect_attempts, options.connect_retry);
    if (!viewer) {
        std::fprintf(stderr, "frame_source: no viewer listening on %s\n", options.socket_name.c_str());
        return 1;
    }
    if (!send_message(viewer.get(), SourceHello{.seed = options.seed, .frame_count = options.frames})) {
        std::fprintf(stderr, "frame_source: viewer went away before the first frame\n");
        return 1;
    }
    FrameGenerator generator{options.seed};
    for (std::uint64_t i = 0; i < options.frames; ++i) {
        const Frame frame = generator.next();
        if (!send_message(viewer.get(), frame)) {
            std::fprintf(stderr, "frame_source: viewer went away at frame %llu\n",
                         static_cast<unsigned long long>(frame.index));
            return 1;
        }
        fillscope_frame_anchor(frame.index);
        std::this_thread::sleep_for(options.interval);
    }
    std::printf("frame_source: last anchored frame %llu\n",
                static_cast<unsigned long long>(last_anchored_frame()));
    return 0;
}

}  // namespace fillscope
