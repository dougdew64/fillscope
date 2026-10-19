#include "lockstep/anchor.h"

#include "common/parse.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <print>
#include <ranges>

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

std::expected<bool, std::error_code> tracer_attached_now() {
    const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected{std::error_code{errno, std::system_category()}};
    }
    // The status file is a few kilobytes, and TracerPid is near its start; one buffer holds it.
    std::array<char, 8192> buffer;
    std::size_t length = 0;
    while (length < buffer.size()) {
        const ssize_t n = ::read(fd, buffer.data() + length, buffer.size() - length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        length += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return tracer_attached(std::string_view{buffer.data(), length});
}

namespace {

void warn_once_that_tracing_is_unknown(const std::error_code& error) {
    static bool warned = false;  // one thread at a time (anchor.h), so a plain flag
    if (!warned) {
        warned = true;
        std::println(stderr, "self-stop: can't tell whether a debugger is attached ({}), so not stopping",
                     error.message());
    }
}

}  // namespace

void set_self_stop(bool on) { g_self_stop = on; }

std::optional<std::uint64_t> last_anchored_frame() {
    return g_anchored ? std::optional<std::uint64_t>{g_last_anchor_index} : std::nullopt;
}

}  // namespace fillscope

// The anchor must survive optimization as a distinct function with a body. Today its one caller is
// in another file and the build doesn't use link-time optimization, so the compiler can neither
// inline it nor merge it; these measures keep that true if either changes:
//   - [[gnu::noinline]]: an inlined self-stop would execute its breakpoint instruction inside the
//     caller, so the debugger would stop there, not here. (The function itself is emitted either
//     way, since it has external linkage, and debuggers bind breakpoints to inlined copies too.)
//   - Its body is unique (no other function stores to g_last_anchor_index), so GCC's -fipa-icf
//     (on at -O2) can't merge it with an identical function, which would make a breakpoint on it
//     fire from unrelated code.
//   - Its store has a reader (last_anchored_frame), and the empty asm statement tells the compiler
//     that memory may be read there, so the store can't be removed.
//   - Its first statement is in this file, so a breakpoint set on it by name opens this file.
extern "C" [[gnu::noinline]] void fillscope_frame_anchor(std::uint64_t frame_index) {
    fillscope::g_last_anchor_index = frame_index;
    fillscope::g_anchored = true;
    asm volatile("" ::: "memory");
    if (!fillscope::g_self_stop) {
        return;
    }
    const auto traced = fillscope::tracer_attached_now();
    if (!traced) {
        fillscope::warn_once_that_tracing_is_unknown(traced.error());
    } else if (*traced) {
        // A breakpoint instruction, executed here, so the debugger stops in this function.
        // raise(SIGTRAP) stopped two frames down inside libc under GDB, and C++26's
        // std::breakpoint() is out of line in libstdc++, so it too would stop inside the library.
        // After int3 the program counter is the next instruction, which is where the debugger
        // reports the stop: the nop keeps that on this line, rather than on the closing brace.
        // frame_index as an input keeps it in a register here, so a debugger can read it even in
        // an optimized build.
#if defined(__x86_64__) || defined(__i386__)
        // ---- the debugger stops here; Continue for the next frame ----
        asm volatile("int3\n\tnop" ::"r"(frame_index));
#else
#error "self-stop needs this architecture's breakpoint instruction, and how its debuggers resume past it"
#endif
    }
}
