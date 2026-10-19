# Running Fillscope

**Purpose:** how to build, run, debug and test Fillscope, and where the code is. **Status:**
procedure. **Read when:** building, running or changing it.

## Where things are
| Path | What it holds |
|---|---|
| `src/channel/` | `frame.h`, the stand-in frame; `message.h`, the messages a source sends (a description of its run, then frames) and their encoding; `net.h`, the socket that carries them |
| `src/lockstep/` | `anchor.h`, `anchor.cpp`: the debugger anchor and the debugger check ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)) |
| `src/viewer/` | `viewer_state.h`, what the viewer has received; `context.h`, what the user points at and follows and the context built from it; `context_publisher.h`, the file it is published to; `viewer_view.h`, what the viewer draws; `viewer_app.h`, its loop; `viewer_options.h`, its command line; `main.cpp`, the window |
| `tools/frame_source/` | a stand-in that produces fake frames until real ones exist; not the matching engine |
| `tools/vscode-debug-state/` | the VS Code extension that publishes the debugger's state ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)) |
| `tests/<area>/` | the tests of one area, mirroring `src/` and `tools/` (`tests/channel/` tests `src/channel/`), with the area's `CMakeLists.txt` |
| `tests/support/` | helpers shared by every area's tests |
| `tests/scripts/` | the commands that run the tests and check them (below) |

## Build
```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
```
Needs GCC, CMake, Ninja, `glfw-devel` and `mesa-libGL-devel`, and Node.js with npm for the
extension's tests. Dear ImGui, GoogleTest and the Dear ImGui Test Engine are fetched at pinned tags;
the extension's development tools are installed by `npm ci` during the build. In VS Code, the default build task does the same and
links `compile_commands.json` for clangd.

## Test
**Iterating and gating are different acts.** Between edits, run only what the edit affects; before
a commit, run everything.
```sh
python3 -I tests/scripts/iterate.py          # between edits: affected fast tests, debug build
python3 -I tests/scripts/iterate.py -R Name  # between edits: fast tests matching a name
tests/scripts/run-tests.sh                   # before a commit: every test, both builds
```
**What `iterate.py` runs.** CMake writes `build/debug/test-manifest.txt`: each test program, its
tier, and the files it uses at run time (a program it starts, a script). A program is affected
when its binary, one of those files, or any `CMakeLists.txt` differs from its last passing run, so
the selection follows the build's own dependencies rather than a hand-kept map. `--list` shows
what's affected without running it; `--all` runs every fast program. `run-tests.sh` records a full
pass, so `iterate.py` starts from there.

**Tiers.** Every test program has one cost tier, set where its area's `CMakeLists.txt` registers it
(`fillscope_gtest` or `fillscope_test_program`, defined in `tests/CMakeLists.txt`), which gives
each of its tests a label and a timeout, so a test that
grows slow fails as slow the day it does:

| Tier | Timeout per test | Run by |
|---|---|---|
| `unit` | 2 s | `iterate.py`, `run-tests.sh` |
| `ui` | 10 s (the whole program) | `iterate.py`, `run-tests.sh` |
| `check` | 30 s (a whole checker run, e.g. a type check) | `iterate.py`, `run-tests.sh` |
| `debugger` | 30 s | `run-tests.sh` |

Every run prints its five slowest tests.

**The test programs:**
- **`channel_tests`, `lockstep_tests`, `frame_source_tests` (GoogleTest, `unit`):** the socket;
  the anchor and the debugger check; the frame source (generation, the command line, a whole run).
- **`lockstep_debugger_tests` (GoogleTest, `debugger`; each test skipped if GDB or LLDB isn't
  installed):** the anchor under GDB and LLDB, driven through the frame source, by breakpoint and
  by self-stop; every stop is in the anchor, in `anchor.cpp`, with the right frame index.

Programs are named for the area they test; when an area's tests span tiers, the slower program
adds the tier to its name.
- **`viewer_tests` (GoogleTest, `unit`):** the viewer's core without Dear ImGui: what it has
  received, the price envelope, the selection, the context built from them, its JSON and bar text,
  publishing (including failure and recovery), and the command line.
- **`viewer_ui_tests` (Dear ImGui Test Engine, no window, `ui`):** the viewer's whole loop, as
  `ViewerApp` runs it for the program, with real clicks: pointing at and following, clearing, a new
  run, the bar and the table's highlights saying exactly what the file holds, a failed write shown
  in the bar and the tooltips, and frames from a real frame source process followed by a second
  source that never describes itself. Text is checked through Dear
  ImGui's logging, since plain text items have no ID for the Test Engine to find; rows, buttons and
  the plot are found by their IDs.
- **`docs_tests` (GoogleTest, `unit`):** every relative link in the repository's Markdown documents
  resolves. Each document is a run-time file of the test, so editing any document re-runs it.
- **`vscode_debug_state_tests` (Node's test runner, `unit`) and `vscode_debug_state_typecheck`
  (the TypeScript compiler, `check`):** the debug-state extension, driven by fake debug sessions;
  and its JavaScript checked against VS Code's and the Debug Adapter Protocol's types.

**CI** ([.github/workflows/ci.yml](../.github/workflows/ci.yml)) runs `run-tests.sh` and both checks
below on every push, in a Fedora container, with `FILLSCOPE_REQUIRE_DEBUGGERS` set so a missing
debugger fails the debugger tests instead of skipping them.

**Checks of the tests themselves** (slower; before changing the test machinery, and in CI):
```sh
python3 -I tests/scripts/mutants.py          # deliberate bugs, each must fail a named test (~2 min)
python3 -I tests/scripts/selection_check.py  # iterate.py selects the right programs for each listed change (~40 s)
```
Both always restore the source. `selection_check.py` uses its own pass record, never the real one.

## Run
Start `build/debug/viewer`, then `build/debug/frame_source`. The frame source first describes its
run (its seed and frame count), then sends five frames a second. When it exits, the viewer goes
back to "waiting"; a new frame source starts a new run, which replaces the old one's frames.
`--exit-after SECONDS` closes the viewer after that long; `--socket NAME` must match the frame
source's.

## Pointing and following
In the viewer's frame table, **click a row to point at its frame**, and **click a frame's id to
follow the id**: every frame with it is highlighted. The context bar at the top says what the
assistant has been given; `x` clears a line. Every change is published at once to
`.fillscope/context.json` in the viewer's working directory (the repository, when started from VS
Code; `--context-file` chooses another path). The file holds a replay address for what is pointed
at: the run's seed and the frame's index, which reproduce that exact state. It also names its
writer (the viewer's process id, when it started, and whether it is still running), so a file left
by a viewer that has exited says so. If a write fails, the bar says so in red, keeps showing what
the file actually holds, and the viewer keeps retrying.

## Debug in lockstep (VS Code, CodeLLDB)
Run and Debug, then one of the two compounds (each builds first):
- **viewer + frame source (self-stop):** the anchor executes a breakpoint instruction when the
  process is traced (by a debugger, here; under another tracer such as `strace`, that ends it).
- **viewer + frame source (breakpoint by name):** LLDB sets a breakpoint on the anchor at launch.

The frame source stops at frame 0. Each Continue (F5) adds one point to the viewer's price line;
F10 from the anchor steps back into the loop, and the point appears when `send` runs. "Viewer
paints" keeps rising while the frame source is stopped.

## The debug-state extension
Installed by linking its folder into VS Code's extensions directory, so it runs straight from the
repository and an edit needs only a window reload (**Developer: Reload Window**):
```sh
ln -s "$PWD/tools/vscode-debug-state" ~/.vscode/extensions/fillscope.fillscope-debug-state-0.1.0
```
Remove the link to uninstall it. Its output channel, **Fillscope debug state**, logs every write,
e.g. `#7 frame source: stopped at anchor.cpp:69 in fillscope_frame_anchor (3 frames, 2 locals)`.
