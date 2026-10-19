// The debugger anchor: the function a process calls after each frame it sends, so that a debugger
// stopped there advances the viewer's animation by one frame per Continue
// (docs/debugger-lockstep.md).
#pragma once

#include <cstdint>
#include <string_view>

namespace fillscope {

// Whether the text of a /proc/<pid>/status file names a tracer (a nonzero TracerPid).
[[nodiscard]] bool tracer_attached(std::string_view proc_status);

// Whether a debugger is attached to this process now.
[[nodiscard]] bool debugger_attached();

// Self-stop: the anchor executes a breakpoint instruction when a debugger is attached, so no
// breakpoint has to be armed. Without a debugger it does nothing.
void set_self_stop(bool on);

// The index most recently passed to the anchor.
[[nodiscard]] std::uint64_t last_anchored_frame();

}  // namespace fillscope

// The anchor. extern "C" so a debugger finds it by a plain name: break fillscope_frame_anchor.
extern "C" void fillscope_frame_anchor(std::uint64_t frame_index);
