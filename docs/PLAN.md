# Plan

**Purpose:** the next steps, in order, and what "done" means for each. **Status:** live plan; a
step is removed when it lands (git keeps it), and the file is deleted when it's empty.
**Read when:** choosing what to work on next.

Decisions behind the steps are in the [charter](CHARTER.md); the lockstep design and its
experiment results are in [debugger-lockstep.md](debugger-lockstep.md). In parallel, the owner
reads the trading domain; the stage-by-stage split of who writes what (charter §4) waits on that.

## 1. Tests for the lockstep experiment
The engine and viewer in [experiments/lockstep/](../experiments/lockstep/README.md) work, checked
by hand. Before building on them, they get automated tests. This step also sets up what every
later step's tests use.
- **Test framework:** GoogleTest, fetched at a pinned release; run through CTest.
- **Sanitizers:** a build with AddressSanitizer and UndefinedBehaviorSanitizer, in which every
  test runs.
- **Seams:** the engine's and viewer's logic moved out of `main` so tests can call it: frame
  generation, the debugger check, the anchor, the socket send and receive, and the viewer's
  drawing as a function of its state.
- **Engine tests:**
  - frames are generated with consecutive indices and the same values for the same seed;
  - the debugger check reads `TracerPid` correctly, from sample `/proc/self/status` text;
  - the anchor records each frame index, and with self-stop on and no debugger it doesn't stop;
  - every frame sent arrives at a listening socket, in order;
  - under GDB and LLDB (run by the test, skipped if the debugger isn't installed): each stop is in
    the anchor, in `engine.cpp`, with the right `frame_index`.
- **Viewer tests, with the Dear ImGui Test Engine** (fetched at the tag matching Dear ImGui; free
  license for open-source projects):
  - the counts on screen equal the frames received, and the last frame's values are shown;
  - with no engine connected it shows "waiting";
  - the price plot appears once frames arrive;
  - frames from a test engine process appear in the viewer, in order.
- **Must-fire:** each test shown to fail when the behaviour it checks is broken on purpose.

**Done when:** `ctest` passes in the normal and sanitizer builds, from one command.

## 2. The project skeleton
A top-level CMake build for Fillscope itself, reusing step 1's test framework and sanitizer
build, plus CI on Linux running the tests on every push.

## 3. The frame channel and viewer shell
The experiment's engine-to-viewer path turned into a reusable library with tests, using stand-in
frames until the message interfaces exist.

## 4. Publishing the debugger's state
A VS Code extension, like HRW's, that writes the stack and locals to a file on every stop, so the
assistant can explain what is being stepped through. The one piece of HRW's debugger
integration Fillscope still needs (see the design note).
