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
under the sanitizers in the sanitizer build; the exchange's multicast reaches the test in CI's container. The
test machinery covers the back end (the sanitizer check, scratch copies, test selection).

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
numbers, a snapshot's content. Nine mutants, all caught. Bugs found and fixed in the fork, each by
a test that failed first: `MEOrderBook`'s table of client orders and its table of price levels
were never initialized; `createSocket()` leaked `getaddrinfo()`'s list. The build file's
`CMAKE_CXX_COMPILER` after `project()` also had to go. Then, at the owner's request (fork
`c7f9b35`), a fourth: prices 256 or more ticks apart shared a price level and could leave a book
crossed; levels are now found by exact price, in a fixed-size hash table (`PriceLevelTable`), so
real prices such as ITCH's work. Known, not fixed (the fork's README): a book holds at most 255
price levels at once; threads are never joined; shutdown clears 2 GB per book. The tests take about 8 s and 3 GB, since the exchange's
objects preallocate their pools, so they run in the `system` tier.

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
remove one. The frame source, its socket and the anchor stay, for the debugger tests, until step 6.
The VS Code configurations now run the viewer, the exchange and a client; lockstep returns in step
6. The exchange gained an option, `--snapshot-interval-seconds`, so tests needn't wait a minute for a
snapshot.
**Status:** built and tested, 2026-10-23: Fillscope `2cc015d`, fork `2b9a550`; both CIs pass. Fillscope's books (`src/feed/`) follow the trading
client's protocol; `feed_system_tests` checks them against the exchange's snapshots, and a late
reader's recovery. Found and fixed on the way: the viewer stopped painting, and so stopped reading
market data and answering SIGTERM, while its window couldn't be seen (vsync); it now paces itself.
Found, recorded in the fork's README: the exchange's multicast goes out on the default interface,
not loopback. **Left:** the owner seeing it live (below).

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
**Status:** built and tested, CI pending. The live run that `feed_system_tests` checks is
recorded, replayed, and matches, byte for byte, every update that arrived. The viewer's replay mode
moves to any update; a point or follow carries the address of the state it describes. Seven mutants,
from the matcher's recording hook to the viewer's backward step, are caught. VS Code: the exchange
records every run; **viewer (replay)** replays the last one and opens it.

### 6. Debugger lockstep with the matcher
The anchor goes into the fork's matcher, called once per request matched, and launch
configurations debug the exchange beside the viewer. The debug-state extension is unchanged.
**The question:** the viewer learns the books only from the market data, which the exchange
publishes from a thread of its own in the same process as the matcher; a debugger stop freezes
every thread, so when the matcher stops after request N, the publisher may not yet have sent N's
updates, and the viewer would show the books before N: not lockstep.
**Decided (the owner, 2026-10-23): lockstep on a replay.** `exchange_replay` is single-threaded, so
it processes request N, sends N's updates to the viewer, then calls the anchor, as the frame source
sends each frame and then calls the anchor; and a run is the same every time it is stepped.
Rejected: lockstep live, the matcher waiting before each stop until the publisher has sent
everything, which changes how the live system runs, and can't be repeated.
**Done when:** stepping the matcher in VS Code, each Continue moves the viewer's book by exactly one
request; the debugger tests check it under GDB and LLDB, as they do the frame source today.
**Status:** not started.

### 7. Attribution in the fork
The attribution report in the fork, with a third column for upstream lines, tested and checked as
Fillscope's is.
**Done when:** the fork publishes its report, and its check runs in its CI.
**Status:** not started.

### 8. The stand-ins retire
The frame source, its socket messages and the stand-in frames go once the viewer runs on the back
end; ARCHITECTURE.md, RUNNING.md and DEBUGGER-LOCKSTEP.md describe the new system.
**Done when:** nothing in Fillscope refers to stand-in frames, and every document is true of the
code.
**Status:** partly done in step 4: the viewer no longer reads frames, and the documents describe the
new system. Left: the frame source, the channel and the anchor, which the debugger tests use until
step 6 moves lockstep to the matcher.

### 9. A comprehensive tech-debt sweep
Once steps 1 to 8 have landed (the owner, 2026-10-23): a sweep of Fillscope and the fork, finding
defects and test gaps, and fixing them, as the earlier sweeps did ([TECH-DEBT.md](TECH-DEBT.md)
keeps what remains).
**Done when:** every item found is fixed or recorded in TECH-DEBT.md with why it waits.
**Status:** not started.

### 10. Faster tests between changes
After steps 1 to 9 (the owner, 2026-10-23), before rapid work on the UI begins. Today `iterate.py`
runs only the affected fast tests, in seconds, but a commit's gate, `run-tests.sh`, takes about 5
minutes (both builds, and the back end's system tests), and the checks of the tests about 20 (the
mutation check about 15). Candidates: a mutation run of only the mutants that name the tests or
code changed (each mutant names the tests that must catch it), itself checked to choose every
mutant a change needs; those checks only in CI, or only when tests change; the sanitizer build only
before a push; the system tests only when the back end or the feed changes.
**Done when:** the time from a change to its tests' result is measured before and after, and each
shortcut is shown not to skip a test the change affects.
**Status:** not started.

## Waiting on the owner
- **A pause once steps 1 to 10 have landed** (the owner, 2026-10-23): the owner learns the work done,
  asking questions, whose answers go into [ARCHITECTURE.md](ARCHITECTURE.md), before the roadmap
  continues.
- **Seeing the book live (step 4's last part):** run **viewer + exchange + client** in VS Code,
  point at and follow orders, and ask the assistant about them ([RUNNING.md](RUNNING.md), "Run" and
  "Pointing and following"). This replaces the viewer shell's test, whose frames are gone.

## Later
The rest of the roadmap becomes changes to the fork, ordered when steps 1 to 10 have landed: ITCH
5.0 replay into the exchange, backtesting on simulated time, fill simulation (queue position,
latency, fees), validation, and measured throughput.
