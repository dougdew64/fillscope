# Running Fillscope

**Purpose:** how to build, run, debug and test Fillscope, and where the code is. **Status:**
procedure. **Read when:** building, running or changing it.

## Where things are
| Path | What it holds |
|---|---|
| `backend/` | the trading system Fillscope observes: a git submodule, the `fillscope` branch of the fork of the system in Ghosh's book, pinned to one commit ([PLAN.md](PLAN.md)); its code is in `backend/Chapter12/`, and its programs are `exchange_main` and `trading_main` |
| `src/feed/` | the exchange's market data as Fillscope reads it: `market_update.h`, an update and its bytes; `feed_socket.h`, a listener on one of the exchange's multicast streams; `book.h`, one ticker's book; `book_builder.h`, every book kept in step with the market data, recovering from snapshots |
| `src/viewer/` | `viewer_state.h`, the books and the ticker shown; `context.h`, what the user points at and follows and the context built from it; `context_publisher.h`, the file it is published to; `viewer_view.h`, what the viewer draws; `viewer_app.h`, its loop; `viewer_options.h`, its command line; `main.cpp`, the window |
| `src/channel/`, `src/lockstep/`, `tools/frame_source/` | the stand-ins the debugger tests still use: a frame source that makes up frames, the socket it sends them on, and the debugger anchor it calls after each ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)) |
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
| `system` | 120 s (the back end: its tests, and its programs running together; a test that runs the exchange holds a CTest lock, since its ports are fixed) | `run-tests.sh` |

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
  numbers, a snapshot's content and its interval, and recording and replaying requests. One process for them all, about 12 s and 4.3 GB (about 40 s in
  the sanitizer build alongside the other tests): the exchange's objects take seconds and
  gigabytes to make. The fork's own CI runs them too.
- **`backend_system_tests` (Python, `system`):** the back end as Fillscope builds it: the exchange
  and a random-trading client run together for about half a minute, and the test, joined to the
  exchange's incremental market data feed, sees orders added and every update arrive in sequence.
  The exchange's ports are fixed in its code, so it fails at once if another exchange is running.
- **`lockstep_debugger_tests` (GoogleTest, `debugger`; each test skipped if GDB or LLDB isn't
  installed):** the anchor under GDB and LLDB, driven through the frame source, by breakpoint and
  by self-stop, at `-O0` and `-O2`; every stop is in the anchor, in `anchor.cpp`, at the right
  line, with the right frame index, and (through the LLDB driver, which acts as the frame source's
  receiver) after that frame was sent.
- **`feed_tests` (GoogleTest, `unit`):** Fillscope's reading of the market data: decoding (the
  back end's own test bytes among others), a book, keeping the books in step and every way recovery
  starts and ends, each order's history, a replay's market data file, and receiving real multicast
  datagrams (test-only groups and ports).
- **`feed_system_tests` (GoogleTest, `system`, the whole program as one test):** the real exchange
  (a snapshot every second) and a client trading at random: every incremental update arrives, the
  books built from updates 1 to S equal each of the exchange's snapshots as of S, a reader that
  joins late recovers and ends with the same books, and the exchange's recording of the run,
  replayed by `exchange_replay`, gives every update that arrived, byte for byte. About half a minute.
- **`viewer_tests` (GoogleTest, `unit`):** the viewer's core without Dear ImGui: the selection, the
  context built from the books and the selection (an order's place in its queue, a level's orders,
  a followed order's events and gaps), its JSON and bar text, publishing (including failure and
  recovery), and the command line.
- **`viewer_ui_tests` (Dear ImGui Test Engine, no window, `ui`):** the viewer's whole loop, as
  `ViewerApp` runs it for the program, with real clicks: the book's order on screen, choosing a
  ticker, pointing at orders and levels, following an order as it fills, clearing, the bar and the
  book's marks (as drawn: each item reports them to the Test Engine) saying exactly what the file
  holds, a failed write shown in the bar and the tooltips, real multicast streams (a feed joined
  while running, recovered from a snapshot, and a datagram that can't be decoded), and a replay's
  controls, with the replay address following the books. (The
  followed row's colour itself isn't observable; its id button reports the mark.) Text is checked
  through Dear ImGui's logging, since plain text items have no ID for the Test Engine to find; rows
  and buttons are found by their IDs.
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
The viewer shows the back end's books. In VS Code: Run and Debug, then **viewer + exchange + client**
(it builds first). It starts the viewer, the exchange (with a snapshot every 5 seconds) and a client
trading at random, each in a debug session of its own; the client waits for the exchange's order
port, which takes the exchange about ten seconds. The back end runs in
`build/debug/backend/Chapter12/`, where it writes its logs. Stopping debugging stops all three.

From a terminal, start the viewer, then the back end as its README says (from
`build/debug/backend/Chapter12/`: `./exchange_main &`, ten seconds, then `./trading_main 5 RANDOM &`;
`--snapshot-interval-seconds S` shortens the exchange's minute between snapshots). The viewer can
start before the exchange or after: started after, it waits for the next snapshot to rebuild the
books (it says "recovering" until then).

**A replay.** The exchange started from VS Code records its requests
(`build/debug/backend/Chapter12/requests.rec`, each run replacing the last). **viewer (replay)**
replays the last recording (`exchange_replay requests.rec market.dat`) and opens it: the controls
move the books to any update of the run. From a terminal: `exchange_main --record FILE`, then
`exchange_replay FILE MARKET_DATA`, then `viewer --replay MARKET_DATA`.

`--exit-after SECONDS` closes the viewer after that long; Ctrl-C and SIGTERM close it the same way,
so its last context file says it has stopped. `--incremental GROUP:PORT` and `--snapshot GROUP:PORT`
choose the market data streams (by default the exchange's). The viewer prints on stderr when
publishing the context fails and when it works again, and if it can't join a stream.

## Pointing and following
The buttons along the top choose the ticker whose book is shown. In the book, **click an order's
row to point at it**, **click a price to point at that level**, and **click an order's id to follow
it**: its row is highlighted, and its history (every update applied to it) is shown below. The
context bar at the top says what the assistant has been given; `x` clears a line. Every change is
published at once to `.fillscope/context.json` in the viewer's working directory (the repository,
when started from VS Code; `--context-file` chooses another path).

The file says what the books hold as of the last update applied (`asOfSeq`): for an order pointed at,
its place in its queue (the orders and quantity ahead of it), or how it left; for a level, its
orders in the order they will fill; for an order followed, its events, and any recoveries whose
updates they may be missing. In a replay, what is pointed at and followed carries a replay address
(the market data file, its recording, and the update the books are after), which rebuilds that
exact state; live, the file says there is none. It names its writer (the viewer's process id, when it started, and whether it is
still running), so a file left by a viewer that has exited says so. If a write fails, the bar says
so in red, keeps showing what the file actually holds, and the viewer keeps retrying.

## Debug in lockstep
Not yet with the exchange: stepping its matcher while the viewer's books follow is step 6 of
[PLAN.md](PLAN.md). The rules it will rest on, and what the debugger tests check with the stand-in
frame source, are in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).

## The debug-state extension
Installed by linking its folder into VS Code's extensions directory, so it runs straight from the
repository and an edit needs only a window reload (**Developer: Reload Window**):
```sh
ln -s "$PWD/tools/vscode-debug-state" ~/.vscode/extensions/fillscope.fillscope-debug-state-0.1.0
```
Remove the link to uninstall it. It starts with the first debug session, and writes only for
sessions in a Fillscope folder; open the folder in one VS Code window only. Its output channel,
**Fillscope debug state**, logs every write, e.g.
`#SEQ SESSION: stopped at FILE:LINE in FUNCTION (N frames, N locals)`, one line per write.
