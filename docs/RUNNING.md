# Running Fillscope

**Purpose:** how to build, run, debug and test Fillscope, and where the code is. **Status:**
procedure. **Read when:** building, running or changing it.

## Where things are
| Path | What it holds |
|---|---|
| `backend/` | the trading system Fillscope observes: a git submodule, the `fillscope` branch of the fork of the system in Ghosh's book, pinned to one commit ([PLAN.md](PLAN.md)); its code is in `backend/Chapter12/`, and its programs are `exchange_main`, `trading_main` and `exchange_replay` |
| `src/feed/` | the exchange's market data as Fillscope reads it: `market_update.h`, an update and its bytes; `feed_socket.h`, a listener on one of the exchange's multicast streams; `book.h`, one ticker's book; `book_builder.h`, every book kept in step with the market data, recovering from snapshots |
| `src/viewer/` | `viewer_state.h`, the books and the ticker shown; `context.h`, what the user points at and follows and the context built from it; `context_publisher.h`, the file it is published to; `viewer_view.h`, what the viewer draws; `viewer_app.h`, its loop; `viewer_options.h`, its command line; `main.cpp`, the window |
| `src/answer/` | the answer tool, `fillscope-answer` (below): `scenario.h`, an answer's requests and claims; `wire.h`, the back end's recording and steps files as bytes; `check.h`, checking a claim; `answer.h`, making an answer |
| `src/common/` | helpers the others share: `fd.h`, a file descriptor's owner; `parse.h`, strict parsing of a number from text; `atomic_file.h`, replacing a file so a reader never sees part of it |
| `tools/vscode-debug-state/` | the VS Code extension that publishes the debugger's state ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)) |
| `tools/attribution/` | `attribution.py`, which writes [ATTRIBUTION.md](ATTRIBUTION.md): per file, the owner's lines and the lines co-authored with Claude (`python3 -I tools/attribution/attribution.py`; `--check` checks the committed report). The back end keeps a copy, which writes its own report with upstream's lines too (`--upstream`; its `CLAUDE.md` gives the command) |
| `tools/code_map/` | `code_map.py`, which checks that [CODE-MAP.md](CODE-MAP.md) puts every code file in exactly one part (`python3 -I tools/code_map/code_map.py --check`) |
| `course/` | the course's lessons, each an answer's scenario, and `course.json`, the manifest that orders them ([ARCHITECTURE.md](ARCHITECTURE.md), §10) |
| `tests/<area>/` | the tests of one area, mirroring `src/` and `tools/` (`tests/feed/` tests `src/feed/`), with the area's `CMakeLists.txt` |
| `tests/support/` | helpers several areas' tests share (running a command, sending to a multicast group) |
| `tests/scripts/` | the commands that run the tests and check them (below) |

## Build
```sh
git submodule update --init                 # once, after cloning: fetches backend/
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFILLSCOPE_WERROR=ON
cmake --build build/debug
```
The back end builds with its own build file and flags (C++20, its own `-Werror`), into
`build/debug/backend/Chapter12/`, and with the sanitizers in the sanitizer build like everything
else. Changing it, step by step: [WORKING-WITH-GIT.md](WORKING-WITH-GIT.md).
Needs GCC, CMake, Ninja, `glfw-devel` and `mesa-libGL-devel`; for the tests, also Python 3, GDB,
LLDB with `python3-lldb`, `flock` (util-linux), and Node.js with npm. Dear ImGui, GoogleTest and
the Dear ImGui Test Engine are fetched as release archives checked against their SHA-256; the
extension's development tools are installed by `npm ci` during the build. In VS Code, the default
build task does the same and links `compile_commands.json` for clangd. A warning in our own code
fails the build, as it does in CI (`-DFILLSCOPE_WERROR`, on by default; written out above, since
a build directory configured before 2026-10-24 remembers it off). The mutation and selection
checks turn it off in their copies: a deliberately broken file may well draw a warning.

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
shared library it loads, a `FILLSCOPE_` environment variable (other than `FILLSCOPE_WERROR` and
the scripts' own, which say where they keep their state), or how CTest runs its tests (command,
environment, timeout) differs from its last passing run; a `CMakeLists.txt` edit counts only
through those. So the selection follows the build's own
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
each of its tests a label and a timeout, so a test that grows slow fails as slow the day it does:

| Tier | Timeout per test | Run by |
|---|---|---|
| `unit` | 2 s | `iterate.py`, `run-tests.sh` |
| `ui` | 10 s (the whole program) | `iterate.py`, `run-tests.sh` |
| `check` | 30 s (a whole run of a checker or a test runner: the type check, the extension's Node tests) | `iterate.py`, `run-tests.sh` |
| `debugger` | 30 s | `run-tests.sh` |
| `system` | 120 s (the back end: its tests, and its programs running together; a test that runs the exchange holds a CTest lock, since the exchange alone takes about 2 GB of memory) | `run-tests.sh` |

**Ports of their own.** Every test that uses the network does so on ports of its own process, so
tests running at once never meet: tests in parallel, two runs of the tests (a run and the mutation
check, say), or a run and a lockstep session in VS Code, which uses the book's ports. A process
claims a numbered slot, 0 to 999, by locking the first file `slot-N` in `fillscope-test-ports`, in
the temporary directory, that no other process holds, and keeps it until it exits (the kernel
releases the lock even if it crashes). Slot N has ports 22000 + 10 N to 22000 + 10 N + 9, clear of
the book's (12345, 20000 and 20001) and below the range Linux hands out by itself (from 32768).
The rule has three copies: `test_port()` in `tests/support/test_support.h`, `testPort()` in the
back end's `tests/test_support.h`, and `claim_slot()` in `tests/backend/backend_system_test.py`;
each has a test that its process holds its slot. The exchange and the trading client take
`--order-port PORT`, `--snapshot GROUP:PORT` and `--incremental GROUP:PORT` for the tests to move
them; without them, they use the book's.

Every passing run prints its five slowest tests. To run one program's tests by hand, select it by
its label: `ctest --test-dir build/debug -L exe:viewer_tests`. (`-R` matches test names, which in a
GoogleTest program are its tests' own names, such as `Publisher.AUserActionIsWrittenAtOnce`.)

**The test programs.** Programs are named for the area they test; when an area's tests span tiers,
the slower program adds the tier to its name.
- **`common_tests` (GoogleTest, `unit`):** the shared helpers: `Fd` closes what it owns, once,
  and a move leaves the source empty; `parse_number` reads a number only when the whole text is
  one, in range.
- **`lowlatency_tests` (GoogleTest, `system`, the whole program as one test):** the back end's own
  tests, in the fork (`backend/Chapter12/tests/`): the market data's layout on the wire, the
  matching (price-time priority, partial fills, sweeps, cancels), the incremental feed's sequence
  numbers, a snapshot's content and its interval, recording and replaying requests, and the order
  server refusing what the matcher can't process. One process for them all, about 3 s and 4.3 GB (about 20 s in the sanitizer build):
  the exchange's objects take gigabytes to make. The fork's own CI runs them too.
- **`lowlatency_allocation_tests` (GoogleTest, `system`, the whole program as one test):** in the
  fork too: what must not allocate (a queue's write and read, a memory pool's allocate and release,
  a price level added and removed, the snapshot synthesizer's updates, and an `ASSERT` that holds),
  counted by its own `operator new`, which is why it is a program apart. A fraction of a second,
  most of it making the snapshot synthesizer.
- **`backend_system_tests` (Python, `system`):** the back end as Fillscope builds it: the exchange
  and a random-trading client run together for a few seconds, and the test, joined to the
  exchange's incremental market data feed on the loopback interface, sees orders added and every
  update arrive in sequence, and none of it on the default interface, which leads off the machine
  (that check is left out on a machine with no network); updates sent on the book's own stream
  meanwhile, as a replay stepped in VS Code would send them, never reach it; an order past the
  order ids, sent to the exchange directly, is rejected and left out of the recording, and the
  exchange runs on and accepts the next; and the exchange refuses bad options, its new ones
  (`--order-port`, `--snapshot`, `--incremental`) among them, and the client a bad
  `--warm-up-seconds`. Its client warms up for a second rather than the book's 10, and its first
  market update must arrive within 9 s, so a warm-up the client ignored fails it.
- **`replay_debugger_tests` (GoogleTest, `debugger`; each test skipped if GDB or LLDB isn't
  installed):** lockstep on a replay: `exchange_replay`, publishing a three-request recording on a
  group of the test's own, under GDB and LLDB, by breakpoint and by self-stop, at `-O0` and `-O2`;
  every stop is in the request anchor, in `lockstep_anchor.cpp`, at the right line, with the
  request's number and the updates published, and (through the LLDB driver, which is the viewer)
  with every update published through that request already arrived.
- **`answer_tests` (GoogleTest, `unit`):** the answer tool's parts: reading a scenario and every
  way one is refused, the bytes of a request, a recording and a steps file, each kind of claim true
  and false, and a replay that isn't the scenario's.
- **`answer_system_tests` (GoogleTest, `system`):** the answer tool run whole, through the real
  `exchange_replay`: an answer written when every claim holds, a false claim refused with the
  earlier answer left as it was, and how the matcher matches (a fill at the resting order's price,
  a sweep across two prices, an order's unfilled rest resting, a cancel, a rejected cancel). About
  8 seconds: each run builds a matching engine.
- **`course_system_tests` (GoogleTest, `system`):** every lesson of the course played through the
  answer tool and the real `exchange_replay`, read back as the viewer reads it with every claim
  checked again, and the manifest naming exactly the lessons `course/` holds. About 10 seconds.
- **`viewer_demo` (the Test Engine's own, `system`):** answers' demo, headless: lesson 6 played in
  the viewer with real clicks, checked at every frame. About 1.5 seconds. Run and Debug, then
  **demo: a market maker**, shows it in a window at a pace to follow, about a minute; **demo:
  record a market maker** records it as a video for sharing, quicker and in a 1280 by 720 window,
  about 80 seconds, to `build/debug/demo-a-market-maker.mp4` (it needs `ffmpeg`).
- **`viewer_log_tests` (GoogleTest, `unit`):** the viewer's log: its names, its lines, keeping the
  newest 20 and nothing else in the directory touched, the 10 MB limit; a crash recorded by a
  process that really dies of it (an abort, an uncaught exception, a segmentation fault, a stack
  overflow; in the sanitizer build, a segmentation fault reported by AddressSanitizer beside the
  log); the feed's and answers' events, each said once; the user's actions.
- **`viewer_program_tests` (Python, `system`):** the viewer program itself, as a user runs it,
  the one test of `src/viewer/main.cpp`: on the real exchange and a client, it takes in their market
  data, paints about 60 times a second, reports once a second, and ends on `--exit-after`, SIGTERM
  and SIGINT with a last context file saying it has stopped; `--screenshot` writes its last frame;
  a bad command line and a replay it can't read are refused. And its log says, each way it ends, how it
  ended; killed outright, it has everything until then; crashed, the crash. About 10 seconds. Its window opens on
  an Xvfb display of the test's own, never on the desktop, where it covered the user's windows. So
  it needs Xvfb (`sudo dnf install xorg-x11-server-Xvfb`); without it the test is skipped, and the
  mutants of `main` go uncaught, except in CI (`FILLSCOPE_REQUIRE_DISPLAY`), which installs Xvfb.
- **`viewer_live_demo` (the Test Engine's own, `system`):** the live demo, headless: the viewer
  on the real exchange and a client trading at random, showing other tickers' books and pointing
  at orders, its books checked against the demo's own. About 10 seconds. **demo: live trading**
  shows it in a window; **demo: record live trading** records its 30 seconds to
  `build/debug/demo-live-trading.mp4`.
- **`feed_tests` (GoogleTest, `unit`):** Fillscope's reading of the market data: decoding (the
  back end's own test bytes among others), a book, keeping the books in step and every way recovery
  starts and ends, each order's history, a replay's market data file, and receiving real multicast
  datagrams (test-only groups and ports).
- **`time_shift_tests` (GoogleTest, `unit`):** what the viewer keeps of the market data: through a
  simulated exchange's feed that starts late, loses updates and swaps two, recovering from
  snapshots, the books after every update the live books were live at are kept, and those near a
  run's edges and its checkpoints, and one in 23 of the rest, rebuilt exactly; every update in a
  gap refused, saying why; arrival times; checkpoints; a replay held the same way.
- **`feed_system_tests` (GoogleTest, `system`, the whole program as one test):** the real exchange
  (a snapshot every second) and a client trading at random: every incremental update arrives, the
  books built from updates 1 to S equal each of the exchange's snapshots as of S, a reader that
  joins late recovers and ends with the same books, and the exchange's recording of the run,
  replayed by `exchange_replay`, gives every update that arrived, byte for byte. About 6 s.
  The exchange and the client run on the test's own ports.
- **`viewer_tests` (GoogleTest, `unit`):** the viewer's core without Dear ImGui: the selection, the
  context built from the books and the selection (an order's place in its queue, a level's orders,
  a followed order's events and gaps), its JSON and bar text, publishing (including failure and
  recovery), an answer (its frames, its claims checked again, whose each order is, and every way
  one is refused), and the command line.
- **`viewer_ui_tests` (Dear ImGui Test Engine, no window, `ui`):** the viewer's whole loop, as
  `ViewerApp` runs it for the program, with real clicks: the book's order on screen, choosing a
  ticker, pointing at orders and levels, following an order as it fills, clearing, the bar and the
  book's marks (as drawn: each item reports them to the Test Engine) saying exactly what the file
  holds, a failed write shown in the bar and the tooltips, real multicast streams (a feed joined
  while running, recovered from a snapshot, and a datagram that can't be decoded), a replay's
  controls, with the replay address following the books, and answers (stepped through, a step's
  question before its explanation, a new one replacing the last, a false claim marked, a stale one
  refused). (The
  followed row's colour itself isn't observable; its id button reports the mark.) Text is checked
  through Dear ImGui's logging, since plain text items have no ID for the Test Engine to find; rows
  and buttons are found by their IDs.
- **`docs_tests` (GoogleTest, `unit`):** every relative link in the repository's Markdown documents
  resolves. A link can name any file, so `iterate.py` runs it every time.
- **`scripts_tests` (Python's unittest, `unit`):** the test machinery's own logic in `iterate.py`:
  reading the manifest, what is never recorded as passed, the registration and sanitizer checks,
  fingerprints and the pass record.
- **`attribution_tests` and `attribution_report_check` (Python, `check`), and
  `attribution_copy_check` (`unit`):** the attribution script's logic, on small repositories of its
  own, a fork's included; that the committed [ATTRIBUTION.md](ATTRIBUTION.md) is exactly what its
  named commit gives; and that the back end's copy of the script is this one. The report's numbers
  depend on the repository's history, so `iterate.py` runs the check every time.
- **`code_map_tests` and `code_map_check` (Python, `unit`):** the code map's check, on small
  repositories of its own; and that [CODE-MAP.md](CODE-MAP.md) puts every code file in exactly one
  part. Which files the repository has is no single file, so `iterate.py` runs the check every
  time.
- **`vscode_debug_state_tests` (Node's test runner, `check`) and `vscode_debug_state_typecheck`
  (the TypeScript compiler, `check`):** the debug-state extension's logic, driven by fake debug
  sessions; its VS Code side, run against a fake `vscode` module; and its JavaScript checked
  against VS Code's and the Debug Adapter Protocol's types.

**CI** ([.github/workflows/ci.yml](../.github/workflows/ci.yml)) runs `run-tests.sh` and both checks
below on every push to `main` and every pull request, in a Fedora container pinned by digest,
with warnings as errors and `FILLSCOPE_REQUIRE_DEBUGGERS` set so a missing debugger fails the
debugger tests instead of skipping them. Every commit pushed to `main` gets a run of its own, in
parallel with any still going; a newer push to a pull request cancels that pull request's run.

**Checks of the tests themselves** (slower; before changing the test machinery, and in CI):
```sh
python3 -I tests/scripts/mutants.py          # deliberate bugs, each must fail the tests it names:
                                             #   those not caught already, as things are
python3 -I tests/scripts/mutants.py --all    #   every one
python3 -I tests/scripts/mutants.py NAME     #   those whose names contain NAME
python3 -I tests/scripts/mutants.py --list   #   which would run, running none
python3 -I tests/scripts/selection_check.py  # iterate.py and mutants.py choose exactly right
```
**How long each takes** (measured on the development machine, with both builds already current):
`run-tests.sh` about 4 minutes (2026-10-25); the selection check about 5 (2026-10-25, with its
mutant cases); the mutation check, every mutant (`--all`, or anything the check's own code
depends on changed), about 17, for 210 mutants (2026-10-25), each a rebuild and a test run (2026-10-24: about
10 minutes of rebuilds, and about 7 of the back end's system tests, which wait more than they
compute). Run as it normally is, it runs only the mutants a change reached: after a change to the
viewer's drawing code, 18 of the 192 in about 40 seconds; with nothing changed, none, in under 2
seconds. A cold build adds several minutes to the first.
Run as root (CI's container), the mutation check skips, and names, the mutants whose test needs a
directory it can't write to, which root always can.
Both work in a copy of the working tree, with a build of their own, so the files being edited, the
build in use and the real pass record are never touched, whatever stops them. The selection check's
copy is temporary; the mutation check keeps its copy and its record of caught mutants in
`build/mutants/` and updates the copy each run (two runs at once wait for each other). A mutant
names the tests meant to catch it: CTest names, single Test Engine tests (`ui:`), single Node tests
(`node:`), or the back end's GoogleTest tests (`lowlatency:`). `mutants.py` first checks that every
test a mutant to run names passes on the real code, so a test that already fails can't make a
mutant look caught. How it chooses which mutants to run is in
[ARCHITECTURE.md](ARCHITECTURE.md) §9, *Whether the tests can be trusted*.

## Run
The viewer shows the back end's books. In VS Code: Run and Debug, then **viewer + exchange + client**
(it builds first). It starts the viewer, the exchange (with a snapshot every 5 seconds) and a client
trading at random, each in a debug session of its own; the client waits for the exchange's order
port, which takes the exchange about a second. The back end runs in
`build/debug/backend/Chapter12/`, where it writes its logs. Stopping debugging stops all three.

From a terminal, start the viewer, then the back end as its README says (from
`build/debug/backend/Chapter12/`: `./exchange_main &`, a second or two, then `./trading_main 5 RANDOM &`;
`--snapshot-interval-seconds S` shortens the exchange's minute between snapshots). The viewer can
start before the exchange or after: started after, it waits for the next snapshot to rebuild the
books (it says "recovering" until then). To stop them from a terminal:
`pkill -INT -x exchange_main` (it shuts down in about 45 seconds, measured in the back end's
README: its code waits 10 seconds before and after deleting its parts, and each book takes seconds
to delete), `pkill -x trading_main`, `pkill -x viewer` (or close its window).

**A replay.** The exchange started from VS Code records its requests
(`build/debug/backend/Chapter12/requests.rec`, each run replacing the last). **viewer (replay)**
replays the last recording (`exchange_replay requests.rec market.dat`) and opens it: the controls
move the books to any update of the run. From a terminal: `exchange_main --record FILE`, then
`exchange_replay FILE MARKET_DATA`, then `viewer --replay MARKET_DATA`.

`--exit-after SECONDS` closes the viewer after that long; Ctrl-C and SIGTERM close it the same way,
so its last context file says it has stopped. With `--exit-after`, `--screenshot FILE` writes the
window, as it was last drawn, to FILE (a PPM image) as the viewer exits. `--incremental GROUP:PORT` and `--snapshot GROUP:PORT`
choose the market data streams (by default the exchange's). The viewer prints on stderr when
publishing the context fails and when it works again, and if it can't join a stream.

**The viewer's log.** Each run writes one, in `.fillscope/logs/` (the `logs` directory beside the
context file), named for when it started and its process id; the newest 20 are kept. It says how
the viewer was started, what happened (the feed's events, answers, every click), each second's
line, and how it ended; after a crash, the crash, with a backtrace. To name the viewer's frames in
a backtrace, give `addr2line` the binary the log names, while it is unchanged:
`addr2line -f -C -p -e build/debug/viewer 0x412069 0x40dafa`. In the sanitizer build, a
segmentation fault is reported by AddressSanitizer instead, in the file beside the log ending in
`.sanitizer.` and the process id. What it records, and why, is in
[ARCHITECTURE.md](ARCHITECTURE.md), §4, *The viewer's log*.

## Answers
Ask a question about the market in the editor's chat; the assistant answers with a scenario and
makes the answer. To see it: Run and Debug, then **viewer (answers)** (it builds first), which
watches `.fillscope/answer.json` and shows each answer written there, a pane beside the book. `>`
steps forward: a step's question, if it asks one, then the step played; `<` back; `|<` and `>|` to
the start and the end. The drop-down at the top of the pane offers "Your latest question" and the
course's lessons (`course/`), in order; nothing is shown until you choose. With "Your latest
question" chosen, leave it open: the next answer replaces the shown one. From a terminal: `cmake --build build/debug --target course` to play the lessons, then `build/debug/viewer
--answer .fillscope/answer.json --course build/debug/course`; `--answer-frame N` opens an answer at
frame N.

`build/debug/fillscope-answer SCENARIO` makes an answer: it plays the scenario's requests through the
back end's matcher and checks its claims; if all hold, it writes `.fillscope/answer.json` (`--answer
FILE` chooses another), with the replay's files in `.fillscope/answer/` beside it. If a claim is
false, or a request is something Ghosh's exchange can't do, it says why and writes nothing. The
format, and how the viewer shows an answer, are in [ARCHITECTURE.md](ARCHITECTURE.md), §10.

## Pointing and following
The buttons along the top choose the ticker whose book is shown. In the book, **click an order's
row to point at it**, **click a price to point at that level**, and **click an order's id to follow
it**: its row is highlighted, and its history (every update applied to it) is shown below. The
context bar at the top says what the assistant has been given; `x` clears a line. Every change is
published at once to `.fillscope/context.json` in the viewer's working directory (the repository,
when started from VS Code; `--context-file` chooses another path).

The file says what the books hold as of the last update applied (`asOfSeq`): for an order pointed
at, its place in its queue (the orders and quantity ahead of it), or how it left (or, if it left
unseen, the recovery it left during, `leftDuringRecovery`); for a level, its
orders in the order they will fill; for an order followed, its events, and any recoveries whose
updates they may be missing. In a replay, what is pointed at and followed carries a replay address
(the market data file, its recording, and the update the books are after), which rebuilds that exact
state; live, the file says there is none. It names its writer (the viewer's process id, when it
started, and whether it is still running), so a file left by a viewer that has exited says so. If a
write fails, the bar says so in red, keeps showing what the file actually holds, and the viewer
keeps retrying.

## Debug in lockstep
Stepping the matcher one request at a time, while the viewer's books follow, runs on a replay of a
recorded run ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)). First record one: run **viewer +
exchange + client** for a while, then stop it. Then Run and Debug, one of the two compounds (each
builds first, and replays `build/debug/backend/Chapter12/requests.rec`):
- **lockstep: viewer + replay (self-stop):** the replay stops itself in the anchor after each
  request when it is traced.
- **lockstep: viewer + replay (breakpoint by name):** LLDB sets a breakpoint on the anchor at launch.

The replay stops after request 1, with the viewer already showing the books after it. Each Continue
(F5) advances them by one request; the anchor's `request_number` and `updates_published` say where
the replay is. The viewer, live, must start before the replay publishes (the compounds start both;
the replay takes a couple of seconds to begin), and no exchange may be running, since the replay
publishes on the exchange's own stream. From a terminal: `exchange_replay RECORDING --publish
233.252.14.3:20001 [--self-stop] [--interval-ms MS]`, with the viewer started first.

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
