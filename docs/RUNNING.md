# Running Fillscope

**Purpose:** how to build, run, debug and test Fillscope, and where the code is. **Status:**
procedure. **Read when:** building, running or changing it.

## Where things are
| Path | What it holds |
|---|---|
| `backend/` | the trading system Fillscope observes: a git submodule, the `fillscope` branch of the fork of the system in Ghosh's book, pinned to one commit ([PLAN.md](PLAN.md)); its code is in `backend/Chapter12/`, and its programs are `exchange_main` and `trading_main` |
| `src/channel/` | `frame.h`, the stand-in frame; `message.h`, the messages a source sends (a description of its run, then frames) and their encoding; `net.h`, the socket that carries them |
| `src/lockstep/` | `anchor.h`, `anchor.cpp`: the debugger anchor and the debugger check ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)) |
| `src/viewer/` | `viewer_state.h`, what the viewer has received; `context.h`, what the user points at and follows and the context built from it; `context_publisher.h`, the file it is published to; `viewer_view.h`, what the viewer draws; `viewer_app.h`, its loop; `viewer_options.h`, its command line; `main.cpp`, the window |
| `tools/frame_source/` | a stand-in that produces fake frames until real ones exist; not the matching engine |
| `tools/vscode-debug-state/` | the VS Code extension that publishes the debugger's state ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)) |
| `tools/attribution/` | `attribution.py`, which writes [ATTRIBUTION.md](ATTRIBUTION.md): per file, the owner's lines and the lines co-authored with Claude (`python3 -I tools/attribution/attribution.py`; `--check` checks the committed report) |
| `tests/<area>/` | the tests of one area, mirroring `src/` and `tools/` (`tests/channel/` tests `src/channel/`), with the area's `CMakeLists.txt` |
| `tests/support/` | helpers shared by every area's tests |
| `tests/scripts/` | the commands that run the tests and check them (below) |

## Build
```sh
git submodule update --init                 # once, after cloning: fetches backend/
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
```
The back end builds with its own build file and flags (C++20, its own `-Werror`), into
`build/debug/backend/Chapter12/`, and with the sanitizers in the sanitizer build like everything
else. Changing it, step by step: [WORKING-WITH-GIT.md](WORKING-WITH-GIT.md).
Needs GCC, CMake, Ninja, `glfw-devel` and `mesa-libGL-devel`; for the tests, also Python 3, GDB,
LLDB with `python3-lldb`, `flock` (util-linux), and Node.js with npm. Dear ImGui, GoogleTest and
the Dear ImGui Test Engine are fetched as release archives checked against their SHA-256; the
extension's development tools are installed by `npm ci` during the build. In VS Code, the default
build task does the same and links `compile_commands.json` for clangd. `-DFILLSCOPE_WERROR=ON`
makes a warning in our own code fail the build, as it does in CI.

## Test
**Iterating and gating are different acts.** Between edits, run only what the edit affects; before
a commit, run everything.
```sh
python3 -I tests/scripts/iterate.py          # between edits: affected fast tests, debug build
python3 -I tests/scripts/iterate.py -R Name  # between edits: fast tests matching a name
tests/scripts/run-tests.sh                   # before a commit: every test, both builds
```
**What `iterate.py` runs.** CMake writes `build/debug/test-manifest.txt`: each test program, its
tier, and the files it uses at run time (a program it starts, a script). A program is affected when
its binary, one of those files (the Node.js the extension's tests run with is one of theirs), a
shared library it loads, a `FILLSCOPE_` environment variable (other than `FILLSCOPE_WERROR`), or any
`CMakeLists.txt` differs from its last passing run, so the selection follows the build's own
dependencies rather than a hand-kept map. `docs_tests` runs every time, since a link can name any
file. `--list` shows what's affected without running it; `--all` runs every fast program. These
forms all build first. Fingerprints are taken before the tests run, so a file edited during a run is
re-tested next time. Nothing is recorded for a program that has no tests or skipped them all.
`run-tests.sh` records a full pass, so `iterate.py` starts from there. Both hold a lock on the build
directory while they build and test it, so two runs never use it at once.

**What `run-tests.sh` also checks:** that every test is registered through
`fillscope_test_program` and runs its program, and every program has a test (otherwise the
selection wouldn't know it, or it would pass having tested nothing); that every program in the
sanitizer build is linked with both AddressSanitizer and UndefinedBehaviorSanitizer, and that
none in the normal build is.

**Tiers.** Every test program has one cost tier, set where its area's `CMakeLists.txt` registers it
(`fillscope_gtest` or `fillscope_test_program`, defined in `tests/CMakeLists.txt`), which gives
each of its tests a label and a timeout, so a test that grows slow fails as slow the day it does.
A program that starts the frame source says so there (`USES_FRAME_SOURCE`), which gives it the
frame source's path and makes the frame source one of its run-time files:

| Tier | Timeout per test | Run by |
|---|---|---|
| `unit` | 2 s | `iterate.py`, `run-tests.sh` |
| `ui` | 10 s (the whole program) | `iterate.py`, `run-tests.sh` |
| `check` | 30 s (a whole run of a checker or a test runner: the type check, the extension's Node tests) | `iterate.py`, `run-tests.sh` |
| `debugger` | 30 s | `run-tests.sh` |
| `system` | 120 s (the back end: its tests, and its programs running together) | `run-tests.sh` |

Every passing run prints its five slowest tests. To run one program's tests by hand, select it by
its label: `ctest --test-dir build/debug -L exe:viewer_tests`. (`-R` matches test names, which in a
GoogleTest program are its tests' own names, such as `Publisher.AUserActionIsWrittenAtOnce`.)

**The test programs.** Programs are named for the area they test; when an area's tests span tiers,
the slower program adds the tier to its name.
- **`channel_tests`, `lockstep_tests`, `frame_source_tests` (GoogleTest, `unit`):** the socket;
  the anchor and the debugger check; the frame source (generation, the command line, a whole run).
- **`lowlatency_tests` (GoogleTest, `system`, the whole program as one test):** the back end's own
  tests, in the fork (`backend/Chapter12/tests/`): the market data's layout on the wire, the
  matching (price-time priority, partial fills, sweeps, cancels), the incremental feed's sequence
  numbers and a snapshot's content. One process for them all, about 8 s and 3 GB (nearly 30 s in
  the sanitizer build alongside the other tests): the exchange's objects take seconds and
  gigabytes to make. The fork's own CI runs them too.
- **`backend_system_tests` (Python, `system`):** the back end as Fillscope builds it: the exchange
  and a random-trading client run together for about half a minute, and the test, joined to the
  exchange's incremental market data feed, sees orders added and every update arrive in sequence.
  The exchange's ports are fixed in its code, so it fails at once if another exchange is running.
- **`lockstep_debugger_tests` (GoogleTest, `debugger`; each test skipped if GDB or LLDB isn't
  installed):** the anchor under GDB and LLDB, driven through the frame source, by breakpoint and
  by self-stop, at `-O0` and `-O2`; every stop is in the anchor, in `anchor.cpp`, at the right
  line, with the right frame index, and (through the LLDB driver, which acts as the viewer) after
  that frame was sent.
- **`viewer_tests` (GoogleTest, `unit`):** the viewer's core without Dear ImGui: what it has
  received, the price envelope, the selection, the context built from them, its JSON and bar text,
  publishing (including failure and recovery), and the command line.
- **`viewer_ui_tests` (Dear ImGui Test Engine, no window, `ui`):** the viewer's whole loop, as
  `ViewerApp` runs it for the program, with real clicks: pointing at and following, clearing, a new
  run, the bar and the table's marks (as drawn: each row reports them to the Test Engine) saying
  exactly what the file holds, a failed write shown in the bar and the tooltips, connections that
  send nothing valid, and frames from a real frame source process followed by a second source that
  never describes itself. (The followed rows' colour itself isn't observable; their id buttons
  report the mark.) Text is checked through Dear ImGui's logging, since plain
  text items have no ID for the Test Engine to find; rows, buttons and the plot are found by their
  IDs.
- **`docs_tests` (GoogleTest, `unit`):** every relative link in the repository's Markdown documents
  resolves. A link can name any file, so `iterate.py` runs it every time.
- **`scripts_tests` (Python's unittest, `unit`):** the test machinery's own logic in `iterate.py`:
  reading the manifest, what is never recorded as passed, the registration and sanitizer checks,
  fingerprints and the pass record.
- **`attribution_tests` and `attribution_report_check` (Python, `check`):** the attribution
  script's logic, on small repositories of its own; and that the committed
  [ATTRIBUTION.md](ATTRIBUTION.md) is exactly what its named commit gives. The report's numbers
  depend on the repository's history, so `iterate.py` runs the check every time.
- **`vscode_debug_state_tests` (Node's test runner, `check`) and `vscode_debug_state_typecheck`
  (the TypeScript compiler, `check`):** the debug-state extension's logic, driven by fake debug
  sessions; its VS Code side, run against a fake `vscode` module; and its JavaScript checked
  against VS Code's and the Debug Adapter Protocol's types.

**CI** ([.github/workflows/ci.yml](../.github/workflows/ci.yml)) runs `run-tests.sh` and both checks
below on every push to `main` and every pull request, in a Fedora container pinned by digest,
with warnings as errors and `FILLSCOPE_REQUIRE_DEBUGGERS` set so a missing debugger fails the
debugger tests instead of skipping them.

**Checks of the tests themselves** (slower; before changing the test machinery, and in CI):
```sh
python3 -I tests/scripts/mutants.py [NAME]   # deliberate bugs, each must fail the tests it names (~10 min)
python3 -I tests/scripts/selection_check.py  # iterate.py selects, runs and records the right programs (~2 min)
```
Both work in a scratch copy of the working tree, with a build of their own, so the files being
edited, the build in use and the real pass record are never touched, whatever stops them; the copy
is deleted afterwards. A mutant names the tests meant to catch it: CTest names, single Test Engine
tests (`ui:`) or single Node tests (`node:`). `mutants.py` first checks that every test it names
passes on the real code, so a test that already fails can't make a mutant look caught; with a
`NAME`, it runs only the mutants whose names contain it.

## Run
Start `build/debug/viewer`, then `build/debug/frame_source`. The frame source waits for the viewer
to accept it, describes its run (its seed and frame count), then sends five frames a second. When
it exits, the viewer goes back to "waiting"; a new frame source starts a new run, which replaces
the old one's frames. A frame source started while another is connected is refused, and says so.
`--exit-after SECONDS` closes the viewer after that long; Ctrl-C and SIGTERM close it the same
way, so its last context file says it has stopped. `--socket NAME` must match the frame source's.
The viewer prints on stderr when publishing the context fails and when it works again.

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

The frame source stops at frame 0. Each Continue (F5) adds one frame to the viewer's table and
price line; F10 from a self-stop steps back into the loop (from a breakpoint by name, through the
rest of the anchor first), and the frame appears when `send` runs. "Viewer
paints" keeps rising while the frame source is stopped.

## The debug-state extension
Installed by linking its folder into VS Code's extensions directory, so it runs straight from the
repository and an edit needs only a window reload (**Developer: Reload Window**):
```sh
ln -s "$PWD/tools/vscode-debug-state" ~/.vscode/extensions/fillscope.fillscope-debug-state-0.1.0
```
Remove the link to uninstall it. It starts with the first debug session, and writes only for
sessions in a Fillscope folder; open the folder in one VS Code window only. Its output channel,
**Fillscope debug state**, logs every write, e.g.
`#7 frame source: stopped at anchor.cpp:LINE in fillscope_frame_anchor (3 frames, 2 locals)`, where
LINE is the anchor's breakpoint instruction for a self-stop.
