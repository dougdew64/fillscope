// A stand-in frame source: produces fake frames and sends them to the viewer, calling the anchor
// after each. It exercises the channel, the viewer and debugger lockstep until real frames exist;
// it is not the matching engine.
#pragma once

#include "channel/frame.h"
#include "channel/net.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <random>
#include <span>
#include <string>

namespace fillscope {

// A uniformly distributed integer in [low, high], drawn from `engine` by a method defined here.
//
// Why not std::uniform_int_distribution: the standard specifies std::mt19937_64's output exactly,
// but leaves a distribution's method to each library. libstdc++ and libc++ differ, and libstdc++
// has changed its own before. A replay address promises that a seed reproduces its frames, so every
// step from seed to frame must be fixed by this code, on any compiler.
//
// The method is rejection sampling. A raw draw is one of 2^64 values; reducing it modulo `span`
// (the number of values in [low, high]) would favour the smallest results whenever 2^64 isn't a
// multiple of span. So the first (2^64 mod span) raw values are rejected and drawn again, leaving a
// whole number of blocks of span values, and every result is exactly equally likely.
//
// A template, so tests can pass a generator whose draws they choose. The constraint asks for a
// standard random bit generator (C++20's std::uniform_random_bit_generator concept) whose every
// draw can be any of all 2^64 values, which the method relies on; std::mt19937_64 is one.
template <typename Generator>
    requires std::uniform_random_bit_generator<Generator> && (Generator::min() == 0) &&
             (Generator::max() == std::numeric_limits<std::uint64_t>::max())
[[nodiscard]] std::uint64_t draw_uniform(Generator& generator, std::uint64_t low, std::uint64_t high) {
    const std::uint64_t span = high - low + 1;  // unsigned arithmetic: wraps to 0 for the full range
    if (span == 0) {
        return generator();
    }
    // 2^64 mod span, computed without 2^64 (which doesn't fit): (0 - span) wraps to 2^64 - span,
    // and (2^64 - span) mod span equals 2^64 mod span.
    const std::uint64_t rejected_below = (0 - span) % span;
    std::uint64_t raw = generator();
    while (raw < rejected_below) {
        raw = generator();
    }
    return low + raw % span;
}

// Produces frames with consecutive indices, a random-walk price and an id drawn from a small pool,
// so each id recurs across many frames. The same seed gives the same frames, on any compiler (see
// draw_uniform), so a run can be repeated exactly.
class FrameGenerator {
public:
    explicit FrameGenerator(std::uint64_t seed) : engine_{seed} {}
    [[nodiscard]] Frame next();

    static constexpr std::int64_t kStartPrice = 10'000;
    static constexpr std::int64_t kMaxPriceStep = 2;
    static constexpr std::uint32_t kMinQty = 1;
    static constexpr std::uint32_t kMaxQty = 500;
    static constexpr std::uint32_t kMinId = 1;
    static constexpr std::uint32_t kMaxId = 20;

private:
    std::mt19937_64 engine_;
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
    // How long to wait, once connected, for the viewer to accept the connection: a viewer takes it
    // on its next paint, and one that has a source already refuses it at once.
    std::chrono::milliseconds accept_timeout{2000};
};

// The options a command line asks for, or why it can't be read: an unknown option, an option
// missing its value, or a value that isn't a number of the right kind. `args` excludes the
// program's name.
[[nodiscard]] std::expected<SourceOptions, std::string> parse_source_args(std::span<const char* const> args);

// Connects to the viewer and waits for it to accept the connection, describes the run (a
// SourceHello with the seed and frame count), then sends the frames, calling the anchor after each
// and waiting the interval between them. Returns the process's exit code: 0, or 1 with the reason
// written to standard error (including a viewer that refused the connection because it has
// another source connected).
[[nodiscard]] int run_frame_source(const SourceOptions& options);

}  // namespace fillscope
