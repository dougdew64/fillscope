// A stand-in frame source: produces fake frames and sends them to the viewer, calling the anchor
// after each. It exercises the channel, the viewer and debugger lockstep until real frames exist;
// it is not the matching engine.
#pragma once

#include "channel/frame.h"
#include "channel/net.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>

namespace fillscope {

// Produces frames with consecutive indices and a random-walk price. The same seed gives the same
// frames, so a run can be repeated exactly.
class FrameGenerator {
public:
    explicit FrameGenerator(std::uint64_t seed) : rng_{seed} {}
    Frame next();

    static constexpr std::int64_t kStartPrice = 10'000;
    static constexpr int kMaxPriceStep = 2;
    static constexpr std::uint32_t kMinQty = 1;
    static constexpr std::uint32_t kMaxQty = 500;

private:
    std::mt19937_64 rng_;
    std::uint64_t next_index_ = 0;
    std::int64_t price_ = kStartPrice;
};

struct SourceOptions {
    std::uint64_t frames = 1000;
    std::chrono::milliseconds interval{200};
    bool self_stop = false;
    std::string socket_name{kDefaultSocketName};
    std::uint64_t seed = 42;
    // How long to wait for a viewer that may still be starting: about five seconds by default.
    int connect_attempts = 50;
    std::chrono::milliseconds connect_retry{100};
};

// The command line, or nothing if it is malformed. args excludes the program name.
[[nodiscard]] std::optional<SourceOptions> parse_source_args(std::span<const char* const> args);

// Connects to the viewer and sends the frames, calling the anchor after each. Returns the exit code.
int run_frame_source(const SourceOptions& options);

}  // namespace fillscope
