#include "lockstep/anchor.h"

#include "common/parse.h"

#include <algorithm>
#include <fstream>
#include <ranges>
#include <sstream>

namespace fillscope {
namespace {

// Process-wide state for the anchor; see anchor.h on threads. Plain variables, and plain stores to
// them, because the anchor's first statement must be code of this file (docs/DEBUGGER-LOCKSTEP.md,
// rule 2). Not std::atomic: its store() is inlined from a libstdc++ header even at -O0, and LLDB's
// breakpoint by name landed in atomic_base.h. Not std::optional either, though "an index, or none"
// is what these two mean together: assigning one runs <optional>'s code first, and under
// AddressSanitizer GDB's breakpoint by name then stopped before the argument was stored, reading
// frame_index as 0 at every stop. last_anchored_frame() builds the optional from them instead.
std::uint64_t g_last_anchor_index = 0;
bool g_anchored = false;
bool g_self_stop = false;

}  // namespace

bool tracer_attached(std::string_view proc_status) {
    constexpr std::string_view key = "TracerPid:";
    // Only a line that *starts* with the key counts. The status file's first line is the process's
    // name, which a program can set to any text, including "TracerPid: 1".
    // std::views::split yields each line as a subrange of characters; std::string_view's range
    // constructor (C++23) views it without copying.
    for (const auto line_range : proc_status | std::views::split('\n')) {
        const std::string_view line{line_range};
        if (!line.starts_with(key)) {
            continue;
        }
        std::string_view value = line.substr(key.size());
        value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
        const auto pid = parse_number<int>(value);
        return pid.has_value() && *pid != 0;
    }
    return false;
}

bool tracer_attached_now() {
    std::ifstream file{"/proc/self/status"};
    std::ostringstream text;
    text << file.rdbuf();
    return tracer_attached(text.str());
}

void set_self_stop(bool on) { g_self_stop = on; }

std::optional<std::uint64_t> last_anchored_frame() {
    return g_anchored ? std::optional<std::uint64_t>{g_last_anchor_index} : std::nullopt;
}

}  // namespace fillscope

// The anchor must survive optimization as a distinct function with a body:
//   - [[gnu::noinline]] keeps calls to it from being inlined, which would leave no function for a
//     breakpoint to bind to.
//   - Its body is unique (no other function stores to g_last_anchor_index), so the linker and
//     GCC's -fipa-icf (on at -O2) can't merge it with an identical function, which would make a
//     breakpoint on it fire from unrelated code.
//   - Its store has a reader (last_anchored_frame), and the asm statement tells the compiler that
//     memory may be read here, so the store can't be removed.
//   - Its first statement is in this file, so a breakpoint set on it by name opens this file.
extern "C" [[gnu::noinline]] void fillscope_frame_anchor(std::uint64_t frame_index) {
    fillscope::g_last_anchor_index = frame_index;
    fillscope::g_anchored = true;
    asm volatile("" ::: "memory");
    if (fillscope::g_self_stop && fillscope::tracer_attached_now()) {
        // A breakpoint instruction, executed here, so the debugger stops in this function.
        // raise(SIGTRAP) stopped two frames down inside libc under GDB, and C++26's
        // std::breakpoint() is out of line in libstdc++, so it too would stop inside the library.
#if defined(__x86_64__) || defined(__i386__)
        asm volatile("int3");  // ---- the debugger stops here; Continue for the next frame ----
#else
#error "self-stop needs this architecture's breakpoint instruction, and how its debuggers resume past it"
#endif
    }
}
