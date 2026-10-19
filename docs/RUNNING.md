# Running Fillscope

**Purpose:** how to build, run, debug and test Fillscope, and where the code is. **Status:**
procedure. **Read when:** building, running or changing it.

## Where things are
| Path | What it holds |
|---|---|
| `src/channel/` | `frame.h`, the frame sent as raw bytes; `net.h`, `net.cpp`, the socket that carries frames to the viewer |
| `src/lockstep/` | `anchor.h`, `anchor.cpp`: the debugger anchor and the debugger check ([debugger-lockstep.md](debugger-lockstep.md)) |
| `src/viewer/` | `viewer_view.h`, `viewer_view.cpp`, what the viewer draws as a function of its state; `main.cpp`, the window |
| `tools/frame_source/` | a stand-in that produces fake frames until real ones exist; not the matching engine |
| `tests/` | the tests, `run-tests.sh` and `mutants.py` (below) |

## Build
```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
```
Needs GCC, CMake, Ninja, `glfw-devel` and `mesa-libGL-devel`. Dear ImGui, GoogleTest and the Dear
ImGui Test Engine are fetched at pinned tags. In VS Code, the default build task does the same and
links `compile_commands.json` for clangd.

## Test
```sh
tests/run-tests.sh                   # every test, normal and sanitizer builds
python3 -I tests/mutants.py          # each deliberate bug is caught by a test
```
- **`fillscope_tests` (GoogleTest):** the socket, the anchor and the debugger check, and the frame
  source (frame generation, the command line, a whole run).
- **Debugger tests** (in `fillscope_tests`; skipped if GDB or LLDB isn't installed): the frame
  source under GDB and LLDB, by breakpoint and by self-stop; every stop is in the anchor, in
  `anchor.cpp`, with the right frame index.
- **`viewer_ui_tests` (Dear ImGui Test Engine, no window):** what the viewer shows, and frames from
  a real frame source process. Text is checked through Dear ImGui's logging, since plain text items
  have no ID for the Test Engine to find; the plot is found by its ID.
- **`mutants.py`:** breaks the code in 14 deliberate ways, one at a time, and requires the named
  tests to fail each time; the source is always restored.

## Run
Start `build/debug/viewer`, then `build/debug/frame_source`. The price line grows five frames a
second. When the frame source exits, the viewer goes back to "waiting" and accepts the next one.

## Debug in lockstep (VS Code, CodeLLDB)
Run and Debug, then one of the two compounds (each builds first):
- **viewer + frame source (self-stop):** the anchor executes a breakpoint instruction when a
  debugger is attached.
- **viewer + frame source (breakpoint by name):** LLDB sets a breakpoint on the anchor at launch.

The frame source stops at frame 0. Each Continue (F5) adds one point to the viewer's price line;
F10 from the anchor steps back into the loop, and the point appears when `send` runs. "Viewer
paints" keeps rising while the frame source is stopped.
