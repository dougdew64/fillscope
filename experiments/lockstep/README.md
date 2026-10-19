# The lockstep experiment

**Purpose:** how to build, run and test the debugger-lockstep experiment. **Status:** procedure;
the questions it answers and its results are in
[docs/debugger-lockstep.md](../../docs/debugger-lockstep.md). **Read when:** running or changing it.

Two processes: `engine` sends a frame (a random-walk price) over a Unix socket, then calls
`fillscope_frame_anchor`; `viewer` draws the frames with Dear ImGui and counts its own paints.
When the engine exits, the viewer goes back to "waiting" and accepts the next one.

| File | What it holds |
|---|---|
| `frame.h` | the frame, sent as raw bytes |
| `net.h`, `net.cpp` | the socket: listen, connect, send, and the viewer's non-blocking receiver |
| `engine_lib.h`, `engine_lib.cpp` | frame generation, the debugger check, the anchor, the engine's run loop |
| `viewer_view.h`, `viewer_view.cpp` | what the viewer draws, as a function of its state |
| `engine.cpp`, `viewer.cpp` | the two programs' `main` |
| `tests/` | the tests (below) |

## Build
```sh
cmake -S experiments/lockstep -B build/lockstep -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/lockstep
```
Needs GCC, CMake, Ninja, `glfw-devel` and `mesa-libGL-devel`. Dear ImGui, GoogleTest and the Dear
ImGui Test Engine are fetched at pinned tags.

## Test
```sh
experiments/lockstep/run-tests.sh                   # every test, normal and sanitizer builds
python3 -I experiments/lockstep/tests/mutants.py    # each deliberate bug is caught by a test
```
- **`lockstep_tests` (GoogleTest):** frame generation, the debugger check, the anchor, the command
  line, the socket, and a whole engine run.
- **Debugger tests** (in `lockstep_tests`; skipped if GDB or LLDB isn't installed): the engine
  under GDB and LLDB, by breakpoint and by self-stop; every stop is in the anchor, in
  `engine_lib.cpp`, with the right frame index.
- **`viewer_ui_tests` (Dear ImGui Test Engine, no window):** what the viewer shows, and frames from
  a real engine process. Text is checked through Dear ImGui's logging, since plain text items have
  no ID for the Test Engine to find; the plot is found by its ID.
- **`mutants.py`:** breaks the code in 14 deliberate ways, one at a time, and requires the named
  tests to fail each time; the source is always restored.

## Run without a debugger
Start `build/lockstep/viewer`, then `build/lockstep/engine`. The price line grows five frames a
second.

## Run in VS Code (CodeLLDB)
Run and Debug, then one of the two compounds (each builds first):
- **viewer + engine (self-stop):** the anchor executes a breakpoint instruction when a debugger is
  attached.
- **viewer + engine (breakpoint by name):** LLDB sets a breakpoint on the anchor at launch.

The engine stops at frame 0. Each Continue (F5) should add one point to the viewer's price line;
F10 from the anchor steps back into the loop, and the point appears when `send` runs. "Viewer
paints" should keep rising while the engine is stopped.
