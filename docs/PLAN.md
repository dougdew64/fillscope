# Plan

**Purpose:** the next steps, in order, what "done" means for each, and how far each has got.
**Status:** live plan; each step is marked when it lands, with its commits, and the file is
deleted when every step has. **Read when:** choosing what to work on next, or checking progress.

Decisions behind the steps are in the [charter](CHARTER.md) and [DECISIONS.md](DECISIONS.md)
(2026-10-23); the lockstep design is in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).

## The change: Fillscope as the front end for a fork of Ghosh's trading system
Fillscope becomes the observatory for an existing back end: the electronic trading system built in
Sourav Ghosh, *Building Low Latency Applications with C++* (Packt, 2023), whose code is
[PacktPublishing/Building-Low-Latency-Applications-with-CPP](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP)
(MIT). The owner learns the domain by reading the book and running and changing its code; Fillscope
shows what that code does and explains it from its exact state.

**What the back end is** (Chapter 12, checked 2026-10-23): an exchange (a matcher, an order server
taking orders over TCP on port 12345, a market data publisher sending snapshots on UDP multicast
233.252.14.1:20000 and incremental updates on 233.252.14.3:20001) and a
trading client (market making, liquidity taking, or random orders, seeded by the client's id).
C++20, CMake, g++ with `-Werror`. Each chapter's folder is a full copy of the system as it stands
then. It has no tests. A run isn't reproducible: threads and the network decide the order things
arrive in.

**Decided (the owner, 2026-10-23):**
1. **The fork is its own public repository**, a GitHub fork of Packt's, with the work on a branch,
   `fillscope`; its `main` follows upstream. Fillscope pins one commit of that branch, so a change
   in the fork never silently changes Fillscope.
2. **Deterministic replay is the first substantial change to the fork:** the exchange records its
   sequenced input and can replay it, giving the same output. Replay addresses and counterfactuals
   rest on it.
3. **Attribution in the fork has three kinds of line:** upstream (Ghosh's, identified by commit:
   any commit in upstream's history), the owner's, and co-authored with Claude.
4. **Testing in the fork:** what Fillscope relies on, and every change made to it; not a retrofit
   of the whole book's code.
5. **A bug in Ghosh's code** is fixed in the fork, with a test, and recorded.
6. **Who writes it:** Claude implements the steps below. The owner's own changes to the fork, made
   while learning, are the owner's, and the owner can claim any step's code before it starts.

## Steps
Each step: what it delivers, what "done" means, and its status. Open questions are settled when
the step is reached, and recorded in DECISIONS.md.

### 1. The fork
Fork the repository on GitHub and create the `fillscope` branch. Work in one folder, `Chapter12`,
the complete system; the other chapters stay as upstream has them. The branch's README says what
the fork is for and links to Fillscope.
**Done when:** the branch builds with Fillscope's toolchain (GCC 16, `-Werror`), any fixes that
needs are committed and listed, and the exchange and a client run together from its scripts.
**Status:** done, 2026-10-23: [dougdew64/Building-Low-Latency-Applications-with-CPP](https://github.com/dougdew64/Building-Low-Latency-Applications-with-CPP/tree/fillscope),
branch `fillscope`, commit `0bb0073`. The one fix: CMake 4 refuses Chapter 12's minimum of 3.0,
now 3.5; the code then builds unchanged with GCC 16.2.1 under its own `-Werror`, at debug and
release. The exchange ran with the market maker and the random client. Found: the published
`run_exchange_and_clients.sh` never finishes by itself (it waits for the clients, which wait for a
silent market); the fork's README says how to run and stop it instead.

### 2. Fillscope builds the pinned back end
The fork joins Fillscope as a git submodule at `backend/`, so it can be read, edited and debugged
in the same VS Code workspace; Fillscope's build builds `exchange_main` and `trading_main` from the
pinned commit, locally and in CI. The attribution report skips the submodule (it is the fork's own
report's job).
**Done when:** a Fillscope test starts the exchange and a client from the pinned commit and sees
orders reach the exchange and market data leave it; CI runs it.
**Status:** done, 2026-10-23: Fillscope `a7ca207`, pinning the fork at `941ef50` (which raised
CMake's minimum to 3.10, to silence CMake 4.3's deprecation warning). `backend_system_tests`, in a
new `system` tier, passes in both builds locally and in CI (about 31 s each), with the back end
under the sanitizers in the sanitizer build; the exchange's multicast reaches the test in CI's
container. The test machinery covers the back end (the sanitizer check, scratch copies, test
selection).

### 3. Tests of what Fillscope relies on
Tests in the fork, run by its own CI and by Fillscope's: the market data messages' layout and
encoding, their sequence numbers, snapshot recovery, and the matching behaviour the viewer will
show (price-time priority, partial fills, cancels).
**Settled:** the tests are in the fork, `Chapter12/tests/`, GoogleTest (the release Fillscope
uses), built only with `-DLOWLATENCY_TESTS=ON`; the fork's CI (`.github/workflows/tests.yml`)
runs them normally and under the sanitizers, and Fillscope runs them as `lowlatency_tests`.
**Done when:** the tests pass on the pinned commit, and mutants of the matcher and the encoding
are caught.
**Status:** done, 2026-10-23: fork `73b4ef3`, Fillscope `ef3da5c`; both CIs pass. 17 tests: the
market data's layout, the matching (price-time priority, partial fills, sweeps, cancels), sequence
numbers, a snapshot's content. Nine mutants, all caught. Bugs found and fixed in the fork, each by a
test that failed first: `MEOrderBook`'s table of client orders and its table of price levels were
never initialized; `createSocket()` leaked `getaddrinfo()`'s list. The build file's
`CMAKE_CXX_COMPILER` after `project()` also had to go. Then, at the owner's request (fork
`c7f9b35`), a fourth: prices 256 or more ticks apart shared a price level and could leave a book
crossed; levels are now found by exact price, in a fixed-size hash table (`PriceLevelTable`), so
real prices such as ITCH's work. Known, not fixed (the fork's README): a book holds at most 255
price levels at once; threads are never joined; shutdown clears 2 GB per book. The tests take about
8 s and 3 GB, since the exchange's objects preallocate their pools, so they run in the `system`
tier.

### 4. The viewer shows the live book
The viewer joins the multicast feed as one more listener, with no change to the back end: it
decodes Ghosh's market updates, rebuilds the book (order by order: the updates carry order ids),
recovers from snapshots after a gap, and draws the book. Pointing and following carry over: point
at an order or a price level, follow an order id. The context the viewer publishes describes the
book, not stand-in frames.
**Done when:** a test runs the exchange and clients and checks that the viewer's book equals the
exchange's own book at the same sequence number; the UI tests click through it; the owner sees it
live.
**Decided while building it (2026-10-23):** the viewer becomes a viewer of the books: frames leave it
now rather than in step 8, since keeping both would have doubled every part of it only for step 8 to
remove one. The frame source, its socket and the anchor stayed, for the debugger tests, until step 8.
The VS Code configurations now run the viewer, the exchange and a client; lockstep returns in step
6. The exchange gained an option, `--snapshot-interval-seconds`, so tests needn't wait a minute for a
snapshot.
**Status:** done, 2026-10-23: Fillscope `2cc015d`, fork `2b9a550`; both CIs pass; the owner has seen
it live. Fillscope's books (`src/feed/`) follow the trading client's protocol; `feed_system_tests`
checks them against the exchange's snapshots, and a late reader's recovery. Found and fixed on the
way: the viewer stopped painting, and so stopped reading market data and answering SIGTERM, while
its window couldn't be seen (vsync); it now paces itself. Found, recorded in the fork's README: the
exchange's multicast goes out on the default interface, not loopback.

### 5. Deterministic replay
In the fork: the exchange records the client requests in the order its sequencer releases them to
the matcher, and a replay mode feeds the matcher from a recording, with no clients and no network.
A replay address becomes (recording, sequence number). The viewer can open a recording and go to
any point in it.
**Done when:** a recorded live run, replayed, gives a byte-identical market data stream (checked by
a test); the viewer publishes replay addresses that rebuild the exact state; mutants of the
recording and of the replay are caught.
**Decided while building it (2026-10-23):** the requests are recorded where the matcher processes
them (its single-threaded loop: the order is final there); replay is a program of its own,
`exchange_replay`, which writes the market data a recording gives to a file, rather than a mode of
`exchange_main`; and the viewer opens that file (`--replay`), not the recording, so Fillscope never
runs the back end's code. A replay address is (market data file, its recording, update N).
**Status:** done, 2026-10-23: Fillscope `09f1df6`, fork `fb01a88`; both CIs pass. The live run that
`feed_system_tests` checks is recorded, replayed, and matches, byte for byte, every update that
arrived. The viewer's replay mode moves to any update; a point or follow carries the address of the
state it describes. Seven mutants, from the matcher's recording hook to the viewer's backward step,
are caught. VS Code: the exchange records every run; **viewer (replay)** replays the last one and
opens it.

### 6. Debugger lockstep with the matcher
The anchor goes into the fork's matcher, called once per request matched, and launch
configurations debug the exchange beside the viewer. The debug-state extension is unchanged.
**The question:** the viewer learns the books only from the market data, which the exchange
publishes from a thread of its own in the same process as the matcher; a debugger stop freezes
every thread, so when the matcher stops after request N, the publisher may not yet have sent N's
updates, and the viewer would show the books before N: not lockstep.
**Decided (the owner, 2026-10-23): lockstep on a replay.** `exchange_replay` is single-threaded, so
it processes request N, sends N's updates to the viewer, then calls the anchor, as the stand-in
frame source sent each frame and then called its anchor; and a run is the same every time it is stepped.
Rejected: lockstep live, the matcher waiting before each stop until the publisher has sent
everything, which changes how the live system runs, and can't be repeated.
**Done when:** stepping the matcher in VS Code, each Continue moves the viewer's book by exactly one
request; the debugger tests check it under GDB and LLDB, as they did the frame source.
**Status:** built and tested, 2026-10-23: Fillscope `44382ac`, fork `8bfd544`; both CIs pass.
`exchange_replay --publish` sends each request's updates,
then calls the back end's anchor (`fillscope_request_anchor`); `replay_debugger_tests` check, under
GDB and LLDB, by breakpoint and self-stop, at `-O0` and `-O2`, that every stop is in the anchor with
its arguments readable and, under LLDB, that the viewer already has every update published through
that request. The viewer, live, followed a replay of 505 updates on the exchange's stream with no
recovery. VS Code: the two "lockstep" compounds. Found on the way: GDB sometimes loses track of the
replay as it exits with its logger threads running, so the tests check the replay's own closing
line, not the debugger's report of the exit. **Done,** 2026-10-24: the owner stepped a recorded run
in VS Code (breakpoint by name), and at a stop the debugger's published state (request 2, update 2
sent) and the viewer's (live at update 2) agreed.

### 7. Attribution in the fork
The attribution report in the fork, with a third column for upstream lines, tested and checked as
Fillscope's is.
**Done when:** the fork publishes its report, and its check runs in its CI.
**Status:** done, 2026-10-23: Fillscope `64e885b`, fork `5caa519`; both CIs pass. Fillscope's script
gained `--upstream` (the commit forked from: its history's lines are upstream's) and `--report`;
files unchanged since upstream are summed without blaming them, so the fork's 456 files take a fifth
of a second. The fork keeps a copy (`tools/attribution/`), which `attribution_copy_check` keeps
identical, and its report, `ATTRIBUTION.md`, which its CI checks against its history. Three mutants
of the upstream mode are caught.

### 8. The stand-ins retire
The frame source, its socket messages and the stand-in frames go once the viewer runs on the back
end; ARCHITECTURE.md, RUNNING.md and DEBUGGER-LOCKSTEP.md describe the new system.
**Done when:** nothing in Fillscope refers to stand-in frames, and every document is true of the
code.
**Status:** done, 2026-10-23. The viewer stopped reading frames in step 4; now the frame source,
its socket (`src/channel/`), its anchor (`src/lockstep/`) and their tests are gone. What the rest
still used moved to `src/common/`: `Fd`, with tests of its own, and the number parser's tests,
which had been the frame source's (`common_tests`). The mutation check's mutants of the anchor, the
driver and the tiers' timeouts now target the request anchor, the replay's driver and the common
tests (131 mutants), and ARCHITECTURE.md §6 explains the request anchor.

### 9. A comprehensive tech-debt sweep
Once steps 1 to 8 have landed (the owner, 2026-10-23): a sweep of Fillscope and the fork, finding
defects and test gaps, and fixing them, as the earlier sweeps did ([TECH-DEBT.md](TECH-DEBT.md)
keeps what remains).
**Done when:** every item found is fixed or recorded in TECH-DEBT.md with why it waits.
**Status:** done, 2026-10-23: four independent reviews (Fillscope's code, the fork's changes, the
test machinery, the documents), each finding confirmed in the code or by running it. Fixed in the
"Third sweep, chunk 1" to "chunk 3" commits and the fork's "Third sweep" commit: among them, a
snapshot that failed to apply left the books half rebuilt; a recovery could go unrecorded; an order
that left during a recovery was said to have left by its last update seen; a recording of a request
the matcher can't process was replayed into undefined behaviour; the exchange's and the replay's
command lines accepted values that overflowed or were misread; the mutation check counted a Node
test name that matched nothing as having run. Each fix has a test, seen to fail first, and a mutant.
Seven findings needed the owner's decision; the first two, live requests the exchange can't process and
a recording that can't be written, were decided on 2026-10-24, and the rest are in TECH-DEBT.md. CI passes at chunk 3 (`4c3506d`)
and the fork's commit (`3a5cc6a`); chunks 1 and 2 failed CI on one mutant that chunk 1's publisher
fix had made uncatchable as root, which chunk 3 fixed.

### 10. Faster tests between changes
After steps 1 to 9 (the owner, 2026-10-23), before rapid work on the UI begins. Today `iterate.py`
runs only the affected fast tests, in seconds, but a commit's gate, `run-tests.sh` (both builds,
and the back end's system tests), and the checks of the tests take minutes, the mutation check the
most ([RUNNING.md](RUNNING.md) has the times). Candidates: a mutation run of only the mutants
that name the tests or code changed (each mutant names the tests that must catch it), itself
checked to choose every mutant a change needs; those checks only in CI, or only when tests change; the sanitizer build only
before a push; the system tests only when the back end or the feed changes. Added 2026-10-24,
once tests had ports of their own (RUNNING.md): the mutation check in parallel workers locally,
each with its own copy and build, limited by memory rather than the network; the mutation check
split across CI runners, a matrix of jobs on separate machines (free for a public repository);
the local builds with CI's warnings as errors, so a warning fails here first; and CI as a
backstop rather than a gate, a change counted done when the local gate passes and CI checked
once afterwards. Measured 2026-10-24, the mutation check takes about as long on the development
machine as in CI, 39 minutes for 181 mutants against 34 to 39: most of it is the back end's tests
waiting (on its threads' start-up sleeps, the client's ten seconds, the exchange starting), not
computing. 28.5 of the 39 minutes are test runs, and the 47 mutants that name the back end's
programs take about 26 of them: each of 36 mutants naming `lowlatency_tests` runs the whole
program, about 21 seconds, and a mutant naming two programs runs them one after the other. CI's
two failures in 65 runs were both mutants the local run hadn't run. The owner discusses the options again when step 10 begins. The owner's direction
(2026-10-24, DECISIONS.md): favor simplicity and confidence in the results; where tests are quick,
don't skip programs to save time; the cost to work on is the mutation check's.
**Done when:** the time from a change to its tests' result is measured before and after, and each
shortcut is shown not to skip a test the change affects.
**Status:** done (2026-10-24). First change: each of the 36 back-end mutants names the
GoogleTest tests that catch it (`lowlatency:FILTER`, found by running each mutant against the whole
program), so it runs those rather than the whole program; and a mutant's programs run in parallel
(`ctest -j`), which bought little, since the CTest lock keeps the two system tests apart. Measured
with every command timed: the mutation check from 39.3 to 23.0 minutes, 181 of 181 caught both
times; the back end's own tests from 16.4 minutes to 2.6. What remains is mostly rebuilds (10.3)
and the system tests (about 7). Second change (2026-10-24, the owner's choice): a mutant is skipped
when nothing its result depends on has changed since it was caught (its definition, the file it
mutates, the fingerprints of the programs it runs, the check's own code), with a record of caught
mutants and a kept, incrementally built copy; the test binaries are fingerprinted by their
contents, whatever their size. Checked by new cases in the selection check (32 of 32 right), unit
tests in `scripts_tests`, and 11 mutants of that logic. Measured: every mutant, 23 minutes for 192;
after a change to the viewer's drawing code, 18 mutants in 42 seconds; nothing changed, under 2
seconds. CI keeps no record, and runs every mutant. Third: the local builds fail on a warning in
our own code, as CI's do (`FILLSCOPE_WERROR` on by default), so a warning is found here first. The
owner judged the rest unjustified now (2026-10-24): shorter waits in the system tests, cheaper
rebuilds and parallel workers would save time mainly in the full check, which runs in CI and when
the check's own code changes; a stretch of work in the back end, where a change still chooses
dozens of mutants, is when to weigh them again. CI is treated as a backstop, a working habit
rather than code: a change is done when the local gate passes, and CI is checked once afterwards.

### 11 to 14. Answers: scenarios the real matcher plays, explained step by step
Decided by the owner on 2026-10-25, in place of the pause planned after step 10: the owner learns
the domain with this, beside Ghosh's book, so it comes first. What it is, and the rules it keeps,
are in [FEATURES.md](FEATURES.md), item 15. Who writes what is decided before step 11's code is
written (charter §4).

### 11. The replay says what each request did
The fork's `exchange_replay` also writes, for each request it replays, the range of updates it
caused and the responses the matcher sent to clients (accepted, filled, canceled, cancel
rejected), with each one's client and order ids. The market data file is unchanged.
**Done when:** a recording's replay gives, for every request, exactly the updates and responses the
matcher produced for it, checked against a live run of the same requests; tested in the fork,
with the format in its README.

### 12. The answer tool
A command-line tool reads a scenario (named clients, and requests in order, each with its
explanation and its claims), writes it as a recording, replays it (step 11), checks every claim
against the real state after its step, and writes `.fillscope/answer.json` for the viewer. A
scenario Ghosh's exchange can't express (a market order, a modify) is refused, saying why; so is
one with a false claim, saying which, what was claimed and what is true.
**Done when:** each kind of claim is checked both ways (a true one passes, a false one is refused),
an impossible request is refused, and the answer file records the replay it rests on, so the viewer
can tell an answer from a stale one.

### 13. The viewer plays an answer
The viewer picks up `.fillscope/answer.json` while it runs, and shows it: the books after each
step, a step at a time forward and back, the step's explanation and its claims (each marked true or
false, as the viewer finds it) in a pane beside the book. The context file names the answer and the
step, so a question asked at a step is about that step.
**Done when:** UI tests step through an answer and check the pane and the books at each step, a new
answer replaces the shown one, and a claim made false by a changed replay shows as false.

### 14. A first answer, and the demo
The first real answers, to the owner's first questions; whatever they show missing is fixed. A
scripted demo plays one, as part of the tests (charter §2, priority 3).
**Done when:** the owner has stepped through answers to the owner's own questions, and the demo runs in
the test suite.

## Waiting on the owner
- **Who writes what in steps 11 to 14** (charter §4).

## Later
The features still to build are in the backlog, [FEATURES.md](FEATURES.md); the next ones are
ordered and broken into steps here when steps 11 to 14 have landed.
