# Troubleshooting the tests and the system

**Purpose:** how problems with Fillscope's tests, its CI and the system under test were tracked
down, step by step, so the same methods can be used again: each case's symptom, the experiments
that narrowed it, the cause, the fix, and what is still open; then the tools and facts they rest on.
**Status:** reference; each case is a record of what was done and measured, dated.
**Read when:** a test fails now and then, a run is slower than it was, something appears that
shouldn't, or the machine runs short; and to learn the methods before that happens.

The method in every case is the same, and worth stating once:
1. **Measure the symptom exactly**, and where it is not: which tests, which build, which machine,
   how often, how far over a limit. A test that takes 2.03 s against a 2 s limit and one that hangs
   are different problems.
2. **Compare with a run that was right.** What changed between them: the code, the machine, the
   load, the order things ran in?
3. **Form a guess, and test it with an experiment that could prove it wrong:** the same run with one
   thing changed, more than once, since a problem that comes and goes can pass by chance.
4. **Say what is established and what is still a guess.** A fix that works without its cause being
   found is recorded as such.
5. **Leave a trace for next time:** a test that fails if the problem returns, a mutant that proves
   the test would notice, a diagnostic that says what happened when it fails, and a note here.

Wrong turns are kept below, as they happened: they are part of how each answer was found.

## The session test (`session_system_tests`)
Several cases below involve one test, so what it is first. `session_system_tests`
(`tests/backend/session_system_test.cpp`; named `strategy_system_tests` until 2026-10-27) records a
session of every kind of client on the real exchange and checks it five ways. It runs
`tools/sessions/record_session.py`, the script the "viewer (session)" launch runs, on ports of its
own: the exchange, recording every request; the book's market maker (client 1), liquidity taker
(client 3) and random client (client 5); and the planted slicer (client 6), working three orders
([ARCHITECTURE.md](ARCHITECTURE.md), §1, *A recorded session*). It records until the maker has sent
300 requests, the taker 20 and the slicer every piece and its cancel, about 40 seconds, then replays
the recording into its market data, steps and truth. The five checks, GoogleTest's
`RecordedSession.*`, all on that one session:
1. **The strategies follow their rules:** every order the maker and the taker sent is the one their
   rule gives, on the market as Fillscope's own books rebuild it, at some moment before the order
   arrived ([ARCHITECTURE.md](ARCHITECTURE.md), §1, *The book's strategies*).
2. **Every order and trade has its client in the truth**, checked against the recording's requests,
   worked out without the replay's own account (§3, *A replay's truth*).
3. **Each sliced order was sent exactly as planned:** its pieces' sizes in order, a cancel of each,
   and each piece its gap after the last, up to the updates it was on its way (§1, *A recorded
   session*).
4. **The truth marks exactly the slicer's trades.**
5. **The recording, replayed again, gives the same files**, byte for byte.

It is heavy: the exchange takes about 2.5 GB and each client 1.5 GB, 8.5 GB in all (measured), and
each of the five programs keeps threads spinning, the book's design for low latency, about 8 CPUs'
worth together. So it runs at the lowest priority, holds the lock every test that runs the exchange
holds, and runs with no other test beside it (case 2). When it fails, its output says how the session
went: `record_session.py`'s own log (`record_session.log`), and which clients recovered from a
snapshot (case 5).

## Case 1: CI runs past their time limit (2026-10-27)
**Symptom.** Two CI runs in a row (commits `513d83a` and `b5d4248`) were cancelled after 60
minutes, the job's limit, part-way through the selection check. Every test had passed and every
mutant was caught: nothing failed; the job ran out of time.

**Measuring it.** The time each step took, from GitHub's record of the run:

    gh run view RUN_ID --json jobs -q '.jobs[0].steps[] | "\(.name)|\(.startedAt)|\(.completedAt)"'

Over the last five runs: the gate (*Every test*) took 5 to 6 minutes, the mutation check 23 to 30;
in the two slow runs, 8.1 and 40.6, and 8.1 and 40.9. Everything was about 1.7 times slower.

**First guess: a slower machine.** CI's machines vary. But tests the commit didn't change were
slower by the same factor (in the sanitizer build, `course_system_tests` 25 s to 42 s,
`answer_system_tests` 22 s to 38 s), and two runs in a row were slow. Not ruled out, but weakened.

**The control experiment.** If the code caused it, the commit before it should still be fast; if the
machines, it should now be slow too. So the last normal run (commit `6913644`, without step 24) was
run again: `gh run rerun RUN_ID`. It took 5.0 and 23.8 minutes: normal. So it was something in the
newer commits, not the machines.

**Splitting a step into its parts.** The gate both builds and tests, so which got slower? The log's
lines are timestamped; the first and last compile lines of each build, and its "tests passed" line,
split it (`gh run view RUN_ID --log` saves the whole log; grep it for `== /`, `[1/` and
`tests passed`):

| | debug build | debug tests | sanitizer build | sanitizer tests |
|---|---|---|---|---|
| normal run | 11 s | 1 min 37 s | 10 s | 3 min 2 s |
| slow run | 58 s | 1 min 48 s | 87 s | 3 min 51 s |

The builds: five to nine times longer, for the same files compiled (284 compile lines against 280).
So the compiles were no longer coming from the cache.

**The cause.** CI compiles through ccache, whose store is saved at the end of each run under the
commit's own key, and a run starts from the newest store saved (`.github/workflows/`, the
`actions/cache` step; its log says which key it restored). The normal re-run restored its own
commit's store: an exact hit. The slow run started before the previous commit's run had saved its
store, so it got an older one, step 23's (`Cache hit for restore-key: ccache-3b3c441...`). And step
24 had changed headers most of the code includes (the viewer's state, and in the fork the trade
engine's, through the slicer's), so most objects were compiled afresh: in both builds, and again in
the mutation and selection checks' copies of the tree, which are at other paths and so share
nothing with the main build in ccache's eyes. The store is also full (5,069 MB of its 5 GB), so new
objects push out old ones as the run goes.

**Fix.** The job's limit is now 90 minutes, with this reason beside it: the limit is there for a run
that hangs, which 90 still catches. **Open:** sharing the cache between the main build and the
checks' copies (ccache's `base_dir`) would remove most of the cost, but `-g` makes ccache hash the
build directory by default, and turning that off changes the source paths in the debug information,
which the debugger tests check.

**Lessons.** A control run (`gh run rerun`) separates "the code" from "the machine" in one step.
Split a slow step into its parts before guessing. The cache step's log says what it restored.

## Case 2: sanitizer tests time out beside the session test (2026-10-27)
**Symptom.** The local gate, which had passed all day, failed in its sanitizer build: three or four
time-limited tests timed out (`TimeShiftBuffer.*`, `BookBuilderAsOf.*`, `KeptFile.ReadWhile...`,
each against 2 s; `viewer_ui_tests` against 10 s), always early in the run (test 13 to 30 of 191).
Not every run; most.

**First wrong turn: other programs.** The system log showed viewer windows opening during the
gate, which I put down to the owner; they were mine (case 3). Checked: `uptime` and
`ps -eo pcpu,rss,etime,comm --sort=-pcpu | head` showed the machine idle, and the timeouts came
back on an idle machine. Ruled out.

**Second: the step's own changes.** Step 24 had added two UI tests and a panel drawn every frame.
Timed alone, each UI test took well under a second (each run by name:
`viewer_ui_tests NAME`), and the whole program 3.45 s; the panel, switched off by an environment
variable in a probe build and compared in alternate runs, cost about 10% (4.7 to 6.2 s without, 5.3
to 5.7 s with). One real cost was found and fixed on the way: the new test checked for each dot that
*shouldn't* be drawn with `ItemExists`, which, for an item that isn't there, runs frames until it
gives up, about 40 times; it now reads the number of dots from the panel's heading. But neither
explained 10 s.

**Third: the lock.** Step 24 had made `lowlatency_tests` take the exchange's lock, changing what ran
beside what. Removed in a probe (the build file edited, `cmake build/asan` to apply it): still
timeouts. Ruled out.

**The experiment that found it.** Which test, run beside the others, does this? Excluding a tier, or
one test, from the same parallel run:

    ctest --test-dir build/asan -j 8 -LE system                    # without the system tier
    ctest --test-dir build/asan -j 8 -E '^session_system_tests$'   # without the session test

Without the session test: two runs, both passed. With it: two runs, three timeouts and one. So the
session test, beside them, slowed them.

**Why, when it runs at the lowest priority.** While it ran:

    ps -eo ni,pcpu,nlwp,rss,comm --sort=-pcpu | grep -E 'exchange_main|trading_main'

showed every program at nice 19, as intended, and each using about 1.5 CPUs: spinning. And the
machine (`lscpu`): an 11th-generation laptop i7 of 4 cores, each two hyperthreads (8 CPUs), with a
turbo clock up to 5 GHz that the chip lowers under sustained load. Low priority keeps the spinning
threads from taking CPU *time* from the tests; it doesn't stop them sharing a test's physical core
(a hyperthread pair shares one core's execution units), or keeping the chip at its power limit,
which lowers every core's clock. A test with a 2 s limit that takes 0.6 s normally has little room
for either.

**Fixes tried.** Keeping the session to one core, CPU 0 and its hyperthread (read from
`/sys/devices/system/cpu/cpu0/topology/thread_siblings_list`, set with `sched_setaffinity`): one run
of two still timed out, since the power limit is the chip's. Running it with no test beside it
(CTest's `RUN_SERIAL`): two runs, both passed, and no slower in all (158 and 160 s, against 175 to
183 s for the failing runs). Kept.

**Open.** Why it began that afternoon: the same session passed the gate earlier in the day. The
margins were thin, and something about the machine's state (likely its temperature, after hours of
test runs) tipped them. Running the session alone removes the margin either way.

**It came back, for one test, the same evening.** With the session test alone, `viewer_ui_tests`
(the UI tests, one CTest test for the whole program, limit 10 s) still timed out, in two gate runs
in a row, though alone it took 3.6 s as before. CTest's own output gives the order tests started
(`Start N:` lines in the gate's log, the sanitizer build's section): it started first, beside the
heaviest moment of the run, eight debugger tests starting GDB and LLDB, the TypeScript type check,
the course's replays and the attribution check, all at once. CTest starts the tests that took
longest last time first, so each timeout made the next run start it at the worst time. Raising the
limit would hide a test that grows slow, which is what the limit is for, so the program was split
in two parts, each a CTest test of its own (`viewer_ui_tests`, the live viewer's;
`viewer_replay_ui_tests`, replays', answers' and sessions'; one program, the second run with
`--replays`): 1.85 s and 1.83 s alone, half the time each.

**Lessons.** Exclude one thing at a time from the same parallel run (`-E`, `-LE`) to find what
interferes. Priority (nice) shares CPU time, not cores or power. On a laptop, sustained load lowers
the clock. The order CTest starts tests in matters, and changes with their last times.

## Case 3: viewer windows open on the desktop (2026-10-27)
**Symptom.** Viewer windows appeared on the owner's desktop during my work, though the tests never
open one: the program test gives the viewer an Xvfb of its own, and the demos draw headless.

**Finding them.** GNOME logs each window it is given (`journalctl --since today | grep "Fillscope
viewer"`): 20 that day, in groups of three five seconds apart, at the times of my screenshot runs
(the slicer's three tickers, one screenshot each). I had wrongly told the owner the windows opened
during the gate were theirs; check your own commands' times first.

**Cause.** I ran the viewer under `xvfb-run`, which points X11 at a virtual screen. But on a Wayland
desktop, the window library (GLFW) tries Wayland first, and Wayland's library finds the desktop's
socket in `XDG_RUNTIME_DIR` even with only `DISPLAY` set, so the window opened on the desktop.
Setting `WAYLAND_DISPLAY` to a name with no socket makes GLFW fail outright, not fall back to X11
(tried: "Wayland: Failed to connect to display").

**Fix.** What `tests/viewer/viewer_program_test.py` already does: `WAYLAND_DISPLAY` unset, and
`XDG_RUNTIME_DIR` an empty directory of its own (mode 0700), so Wayland finds nothing and GLFW uses
X11, the Xvfb:

    rt=$(mktemp -d); chmod 700 $rt
    env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=$rt xvfb-run -a -s "-screen 0 1800x1400x24" \
        build/debug/viewer --session data/sessions/latest --ticker 1 --at 400 --exit-after 2 --screenshot shot.ppm

Checked: the screenshot was taken, and the log's count of windows didn't change.

## Case 4: a kept-file test stalls on CI now and then (2026-10-26 to 27)
**Symptom.** `KeptFile.ReadWhileBeingWrittenItIsWhatWasWrittenToTheLastWholeRecord` timed out (2 s)
in four CI runs over two days, in the sanitizer build, where it normally takes 0.6 s; never
locally. Later `TruthFile.SaysWhyItCantBeRead` too.

**Ruled out:** another test's CPU (the suite on four cores, as CI's, `taskset -c 0-3`), its files'
place (on disk rather than in memory: `TMPDIR` set to a disk directory), and memory (a cap of 8.5 GB
with `systemd-run --user --scope -p MemoryMax=8500M`, beside `lowlatency_tests` at its 7.9 GB peak):
none reproduced it.

**A diagnostic, since it couldn't be reproduced.** The test was made to print its progress with the
time since it began, flushed after each line, which CTest shows only when a test fails; and the gate
to list the tests nearest their time limits. The next failure on CI showed every *write* of a cut
file slow, 10 to 100 ms from the 15th on, and every read fast.

**Cause.** Both tests rewrote one file, truncating it, a hundred times or more. On CI the temporary
directory is ext4 (under the container's overlay); ext4 starts writing a file truncated and rewritten
as soon as it is closed (its `auto_da_alloc` guard), and truncating it again waits for that write,
which on a busy disk is slow. Here, `/tmp` is in memory and the build disk btrfs, neither of which
does it. Confirmed on an ext4 file system made here (a 2 GB image, `mkfs.ext4 -E
root_owner=$(id -u):$(id -g)`, attached with `udisksctl loop-setup` and mounted with `udisksctl
mount`) with three programs writing to it meanwhile: the test took 675 to 885 ms against 278 idle,
and a single rewrite waited up to 204 ms, a write to a new file at most 0.2 ms.

**Fix.** Each version written to a file of its own: 278 ms busy or idle. In
[ARCHITECTURE.md](ARCHITECTURE.md), §9, *Tests that write files*.

**Lessons.** What can't be reproduced can be made to explain itself the next time it happens. CI's
file system isn't the one here.

## Case 5: a session's clients recover at its start (2026-10-27)
**Symptom.** The session test failed now and then: once, the market maker stopped by itself (exit
status 0, which the book's client gives only after a minute without market data); once, the maker
sent an order no moment of the market explains and the slicer sent a piece before its gap had
passed.

**Finding it.** A client's market data consumer logs `Packet drops on incremental socket` as it
starts to recover from a snapshot, with the update it expected and the one it got
(`trading_market_data_consumer_N.log`). Under a CPU load (eight busy loops,
`timeout 200 sh -c 'while :; do :; done' &`, never more exchanges: case 6), three sessions in turn:
two clients recovered in each, every time expecting update 1. So not lost datagrams mid-session:
clients joining after the market had begun.

**Cause.** The market's first update is the random client's first order, a second after it starts;
on a busy machine the other clients weren't listening yet. A recovering client's strategy is called
on each of the snapshot's orders as its book is rebuilt, deciding on a book half built, and the
slicer counts them as updates; with a snapshot a minute apart, the maker waited long enough to stop.

**Fix.** The script starts the random client only once the others are listening (a client's consumer
log gets its first line once its thread runs, after it has joined): none recovered in three sessions
under the same load. The exchange also sends a snapshot every second, and `session.json` says which
clients recovered, which the test prints, so a failure says so.

## Case 6: the machine runs out of memory (2026-10-27)
**Symptom.** Four sessions recorded at once, to load the machine for case 5, killed VS Code: the
kernel's OOM killer (`journalctl -k | grep -i "killed process"`).

**Measuring it.** A session's memory, summed over its programs while it runs:

    ps -C exchange_main,trading_main -o rss= | awk '{s+=$1} END {print s/1024 " MB"}'

and per program (`ps -o rss=,args=`): the exchange 2.7 GB, each client 1.5 GB (every book
preallocated for a million orders), 11.7 GB for six clients; four sessions, about 47 GB, on a
machine of 31 GB. The sanitizer build is about the same (measured: 2.0 GB for an exchange started
alone, 1.5 GB a client).

**Fix.** A session now runs one slicer client working three orders rather than three clients: 8.5
GB (measured). `lowlatency_tests` (7.9 GB in the sanitizer build) holds the exchange's lock, so no
session runs beside it on CI's 16 GB. And before starting several of these at once: add them up.

## Case 7: the slicer empties one side of the market (2026-10-27)
**Symptom.** The trades panel's first screenshots: on the slicer's tickers, later in the session,
one side of the book empty and the slicer's pieces resting and being met, not taking.

**Measuring it.** For each ticker, the share of updates after which both sides have orders, and the
slicer's pieces that traded, from the session's market data and truth (a short script reading both
files' layouts; [ARCHITECTURE.md](ARCHITECTURE.md), §3). With the slicer's pieces the market maker's
whole clip, its tickers had both sides quoted after 41 to 71% of their updates, against 90 to 99%
elsewhere.

**Cause.** Two. The slicer left each piece's rest resting, above the market it had pushed, where the
others' orders met it rather than rested; and each piece took the maker's whole quote, and the maker
quotes only while both sides have orders.

**Fix.** Each piece cancelled once accepted (as an immediate-or-cancel order would be), and half the
maker's clip: 66 to 88% (one session). A piece that finds the other side empty fills nothing.

## Tools and facts
**CI** (GitHub's `gh` tool):
- `gh run list --commit FULL_SHA`: a commit's runs. The full SHA: a short one finds nothing.
- `gh run watch RUN_ID --exit-status`: wait for a run. One at a time: several at once hit GitHub's
  rate limit.
- `gh run view RUN_ID --json jobs -q ...`: each step's start and end, so each step's time.
- `gh run view RUN_ID --log > file`: the whole log, timestamped, to grep.
- `gh run rerun RUN_ID`: the control experiment, the same commit again.

**Tests** (CTest):
- `ctest --test-dir build/asan -j 8`: the sanitizer build's tests, eight at a time, as the gate runs
  them.
- `-R REGEX` runs only the tests named; `-E REGEX` all but them; `-L`/`-LE LABEL` by tier
  (`unit`, `ui`, `system`, ...) or program (`exe:NAME`).
- `-V` prints each test's output; `--output-on-failure` only a failure's.
- A GoogleTest program runs one test with `--gtest_filter=Suite.Name`; `viewer_ui_tests` takes a
  name filter as its argument.
- The gate (`tests/scripts/run-tests.sh`) lists the slowest tests and those nearest their limits:
  compare them across runs.
- A test's properties: `RESOURCE_LOCK` (tests holding the same lock never overlap), `RUN_SERIAL`
  (nothing beside it), `TIMEOUT` (set by its tier).

**The machine:**
- `lscpu`: cores, hyperthreads, clock. `cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list`:
  which CPUs share a core.
- `ps -eo ni,pcpu,nlwp,rss,comm --sort=-pcpu`: priority, CPU use, threads and memory of each
  program. `uptime`: the load averaged over 1, 5 and 15 minutes.
- `free -m`: memory; `/usr/bin/time -v PROGRAM`: a program's peak memory ("Maximum resident set
  size").
- `systemd-run --user --scope -p MemoryMax=8500M -p MemorySwapMax=0 COMMAND`: run under a memory cap.
- `taskset -c 0-3 COMMAND`: keep a command to some CPUs, as a smaller machine would.
- `journalctl --since today | grep TEXT`: the system's log, windows opened among it;
  `journalctl -k`: the kernel's, the OOM killer among it.
- `stat -f -c %T DIR`: a directory's file system (tmpfs, btrfs, ext4).
- `/tmp` here is in memory (tmpfs): what a test leaves there, or writes without bound, is memory.

**Pitfalls met:**
- `pgrep -f TEXT` and `pkill -f TEXT` match the shell running them if its command line contains
  TEXT: a `pkill` killed its own shell, and a wait loop on `pgrep` never ended.
- A probe build changes the tree: copy the file first, put it back after, and check
  `git status` shows no trace.
- A guess written down as a fact misleads the next reader: say "likely" until an experiment has
  shown it.
