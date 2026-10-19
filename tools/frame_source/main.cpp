// The stand-in frame source: produces frames, sends each to the viewer, then calls the anchor.
//
// Under a debugger, the anchor is where execution stops after every frame, so each Continue
// advances the viewer's animation by exactly one frame. The viewer is a separate process, so it
// keeps painting while this one is stopped.
//
//   frame_source [--frames N] [--interval-ms MS] [--socket NAME] [--seed S] [--self-stop]
//
// --self-stop: the anchor executes a breakpoint instruction when the process is being traced, so
// no breakpoint has to be armed. Without a tracer it does nothing. Use it only under a debugger.

#include "frame_source.h"

#include <print>
#include <span>

int main(int argc, char** argv) {
    // std::span views argv's elements after the program's name, without copying them. (argc is at
    // least 1 on Linux since 5.18, which always passes the name; the guard keeps the range valid
    // even where it isn't.)
    const auto args = argc > 0 ? std::span<const char* const>{argv + 1, argv + argc} : std::span<const char* const>{};
    const auto options = fillscope::parse_source_args(args);
    if (!options) {
        std::println(stderr, "frame_source: {}", options.error());
        std::println(stderr, "usage: frame_source [--frames N] [--interval-ms MS] [--socket NAME] [--seed S] [--self-stop]");
        return 2;
    }
    return fillscope::run_frame_source(*options);
}
