#include "lockstep/anchor.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace fillscope {
namespace {

// Plain, not std::atomic: only the calling thread touches these. An atomic's store() is inlined
// from a libstdc++ header even at -O0, so LLDB's breakpoint on the anchor by name landed in
// atomic_base.h.
std::uint64_t g_last_anchor_index = 0;
bool g_self_stop = false;

}  // namespace

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

}  // namespace fillscope

// It must keep a body after optimization: an empty function can be merged with every other empty
// function (GCC's -fipa-icf, on at -O2), and a breakpoint on it would then fire from unrelated
// code. The store has a reader (last_anchored_frame), and the asm statement tells the compiler
// memory may be read here, so the store cannot be removed. Its first statement is in this file, so
// a breakpoint set on it by name opens this file.
extern "C" [[gnu::noinline]] void fillscope_frame_anchor(std::uint64_t frame_index) {
    fillscope::g_last_anchor_index = frame_index;
    asm volatile("" ::: "memory");
    if (fillscope::g_self_stop && fillscope::debugger_attached()) {
        // A breakpoint instruction, executed here; raise(SIGTRAP) stopped two frames down inside
        // libc under GDB.
        asm volatile("int3");  // ---- the debugger stops here; Continue for the next frame ----
    }
}
