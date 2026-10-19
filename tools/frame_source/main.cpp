// The stand-in frame source: produces frames, sends each to the viewer, then calls the anchor.
//
// Under a debugger, the anchor is where execution stops after every frame, so each Continue
// advances the viewer's animation by exactly one frame. The viewer is a separate process, so it
// keeps painting while this one is stopped.
//
//   frame_source [--frames N] [--interval-ms MS] [--socket NAME] [--seed S] [--self-stop]
//
// --self-stop: the anchor executes a breakpoint instruction when a debugger is attached, so no
// breakpoint has to be armed. Without a debugger it does nothing.

#include "frame_source.h"

#include <cstdio>
#include <span>

int main(int argc, char** argv) {
    const auto options = fillscope::parse_source_args(std::span<const char* const>{argv + 1, argv + argc});
    if (!options) {
        std::fprintf(stderr, "usage: frame_source [--frames N] [--interval-ms MS] [--socket NAME] [--seed S] "
                             "[--self-stop]\n");
        return 2;
    }
    return fillscope::run_frame_source(*options);
}
