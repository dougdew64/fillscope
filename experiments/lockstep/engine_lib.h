// The engine's logic, separate from main so tests can call it.
#pragma once

#include "frame.h"
#include "net.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace lockstep {

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

// Whether the text of a /proc/<pid>/status file names a tracer (a nonzero TracerPid).
[[nodiscard]] bool tracer_attached(std::string_view proc_status);

// Whether a debugger is attached to this process now.
[[nodiscard]] bool debugger_attached();

// Self-stop: the anchor executes a breakpoint instruction when a debugger is attached.
void set_self_stop(bool on);

// The index most recently passed to the anchor.
[[nodiscard]] std::uint64_t last_anchored_frame();

struct EngineOptions {
    std::uint64_t frames = 1000;
    std::chrono::milliseconds interval{200};
    bool self_stop = false;
    std::string socket_name{kDefaultSocketName};
    std::uint64_t seed = 42;
};

// The command line, or nothing if it is malformed. args excludes the program name.
[[nodiscard]] std::optional<EngineOptions> parse_engine_args(std::span<const char* const> args);

// Connects to the viewer and sends the frames, calling the anchor after each. Returns the exit code.
int run_engine(const EngineOptions& options);

}  // namespace lockstep

// The breakpoint anchor, called after each frame is sent. extern "C" so a debugger finds it by a
// plain name: break fillscope_frame_anchor.
extern "C" void fillscope_frame_anchor(std::uint64_t frame_index);
