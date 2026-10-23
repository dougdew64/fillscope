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
233.252.14.1:20000 and incremental updates on 233.252.14.3:20001, on the loopback interface) and a
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
**Status:** not started.

### 3. Tests of what Fillscope relies on
Tests in the fork, run by its own CI and by Fillscope's: the market data messages' layout and
encoding, their sequence numbers, snapshot recovery, and the matching behaviour the viewer will
show (price-time priority, partial fills, cancels).
**Open:** where the fork's tests live and how its CI is set up; whether they use GoogleTest, as
Fillscope's do.
**Done when:** the tests pass on the pinned commit, and mutants of the matcher and the encoding
are caught.
**Status:** not started.

### 4. The viewer shows the live book
The viewer joins the multicast feed as one more listener, with no change to the back end: it
decodes Ghosh's market updates, rebuilds the book (order by order: the updates carry order ids),
recovers from snapshots after a gap, and draws the book. Pointing and following carry over: point
at an order or a price level, follow an order id. The context the viewer publishes describes the
book, not stand-in frames.
**Done when:** a test runs the exchange and clients and checks that the viewer's book equals the
exchange's own book at the same sequence number; the UI tests click through it; the owner sees it
live.
**Status:** not started.

### 5. Deterministic replay
In the fork: the exchange records the client requests in the order its sequencer releases them to
the matcher, and a replay mode feeds the matcher from a recording, with no clients and no network.
A replay address becomes (recording, sequence number). The viewer can open a recording and go to
any point in it.
**Done when:** a recorded live run, replayed, gives a byte-identical market data stream (checked by
a test); the viewer publishes replay addresses that rebuild the exact state; mutants of the
recording and of the replay are caught.
**Status:** not started.

### 6. Debugger lockstep with the matcher
The anchor goes into the fork's matcher, called once per request matched, and launch
configurations debug the exchange beside the viewer. The debug-state extension is unchanged.
**Open:** the market data publisher is a thread in the exchange's process, so a stop freezes it
too; the update for request N must reach the viewer before the stop. Either the anchor is reached
only once the publisher has sent it, or lockstep runs on a replay (step 5), where the matcher can
wait for the publisher.
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
**Status:** not started.

## Waiting on the owner
- **The viewer shell's test:** pointing and following in the viewer and asking the assistant about
  it ([RUNNING.md](RUNNING.md), "Pointing and following"). Still useful before step 4 rebuilds the
  viewer's content.

## Later
The rest of the roadmap becomes changes to the fork, ordered when steps 1 to 8 have landed: ITCH
5.0 replay into the exchange, backtesting on simulated time, fill simulation (queue position,
latency, fees), validation, and measured throughput.
