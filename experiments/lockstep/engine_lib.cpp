#include "engine_lib.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace lockstep {
namespace {

// Plain, not std::atomic: only the engine's one thread touches these. An atomic's store() is
// inlined from a libstdc++ header even at -O0, so a breakpoint on the anchor by name landed in
// atomic_base.h.
std::uint64_t g_last_anchor_index = 0;
bool g_self_stop = false;

}  // namespace

Frame FrameGenerator::next() {
    std::uniform_int_distribution<int> step{-kMaxPriceStep, kMaxPriceStep};
    std::uniform_int_distribution<std::uint32_t> qty{kMinQty, kMaxQty};
    price_ += step(rng_);
    return Frame{next_index_++, price_, qty(rng_), 0};
}

bool tracer_attached(std::string_view proc_status) {
    constexpr std::string_view key = "TracerPid:";
    const auto at = proc_status.find(key);
    if (at == std::string_view::npos) {
        return false;
    }
    std::string_view rest = proc_status.substr(at + key.size());
    rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
    int pid = 0;
    const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), pid);
    return error == std::errc{} && pid != 0;
}

bool debugger_attached() {
    std::ifstream file{"/proc/self/status"};
    std::ostringstream text;
    text << file.rdbuf();
    return tracer_attached(text.str());
}

void set_self_stop(bool on) { g_self_stop = on; }

std::uint64_t last_anchored_frame() { return g_last_anchor_index; }

std::optional<EngineOptions> parse_engine_args(std::span<const char* const> args) {
    EngineOptions options;
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

int run_engine(const EngineOptions& options) {
    set_self_stop(options.self_stop);
    const Fd viewer = connect_to(options.socket_name, 50, std::chrono::milliseconds{100});
    if (!viewer) {
        std::fprintf(stderr, "engine: no viewer listening on %s\n", options.socket_name.c_str());
        return 1;
    }
    FrameGenerator generator{options.seed};
    for (std::uint64_t i = 0; i < options.frames; ++i) {
        const Frame frame = generator.next();
        if (!send_frame(viewer.get(), frame)) {
            std::fprintf(stderr, "engine: viewer went away at frame %llu\n",
                         static_cast<unsigned long long>(frame.index));
            return 1;
        }
        fillscope_frame_anchor(frame.index);
        std::this_thread::sleep_for(options.interval);
    }
    std::printf("engine: last anchored frame %llu\n",
                static_cast<unsigned long long>(last_anchored_frame()));
    return 0;
}

}  // namespace lockstep

// The breakpoint anchor. It must keep a body after optimization: an empty function can be merged
// with every other empty function (GCC's -fipa-icf, on at -O2), and a breakpoint on it would then
// fire from unrelated code. The store has a reader (last_anchored_frame), and the asm statement
// tells the compiler memory may be read here, so the store cannot be removed. Its first statement
// is in this file, so a breakpoint set on it by name opens this file.
extern "C" [[gnu::noinline]] void fillscope_frame_anchor(std::uint64_t frame_index) {
    lockstep::g_last_anchor_index = frame_index;
    asm volatile("" ::: "memory");
    if (lockstep::g_self_stop && lockstep::debugger_attached()) {
        // A breakpoint instruction, executed here; raise(SIGTRAP) stopped two frames down inside
        // libc under GDB.
        asm volatile("int3");  // ---- the debugger stops here; Continue for the next frame ----
    }
}
