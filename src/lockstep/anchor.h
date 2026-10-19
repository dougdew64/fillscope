// The debugger anchor: the function a process calls after each frame it sends, so that a debugger
// stopped there advances the viewer's animation by one frame per Continue
// (docs/DEBUGGER-LOCKSTEP.md).
//
// The anchor's state is process-wide and unsynchronized: call it, and set_self_stop, from one
// thread at a time. (The frame source does, from its one sending thread.)
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace fillscope {

// Whether the text of a /proc/<pid>/status file names a tracer: a "TracerPid:" line with a nonzero
// process id.
[[nodiscard]] bool tracer_attached(std::string_view proc_status);

// Whether this process is being traced now. A debugger traces the process it debugs, but so do
// other tools, such as strace and rr: Linux reports a tracer, not what kind of tool it is.
[[nodiscard]] bool tracer_attached_now();

// Self-stop: when on, the anchor executes a breakpoint instruction if the process is being traced,
// so a debugger stops there with no breakpoint armed. Without a tracer it does nothing. Under a
// tracer that isn't a debugger (strace, rr), the breakpoint instruction's signal ends the process,
// so self-stop is for running under a debugger only.
void set_self_stop(bool on);

// The index most recently passed to the anchor; std::nullopt if it hasn't been called. (An
// optional, because 0 is a frame index too: "frame 0" and "none yet" must not look alike.)
[[nodiscard]] std::optional<std::uint64_t> last_anchored_frame();

}  // namespace fillscope

// The anchor. extern "C" so a debugger finds it by a plain name: break fillscope_frame_anchor.
extern "C" void fillscope_frame_anchor(std::uint64_t frame_index);
