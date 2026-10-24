# Debugger lockstep

**Purpose:** how a debugger steps the matcher, one request at a time, while the viewer's books
follow, and the rules that keep it working. **Status:** reference. **Read when:** changing the
replay, the anchor, the process split, or the debugger integration.

How to run it is in [RUNNING.md](RUNNING.md), "Debug in lockstep".

## How it works
**Lockstep runs on a replay** (decided 2026-10-23, [DECISIONS.md](DECISIONS.md)). The live exchange
publishes its market data from a thread of its own, in the matcher's process, and a debugger stop
freezes every thread, so a viewer could lag a stop there. A replay of a recorded run
([ARCHITECTURE.md](ARCHITECTURE.md), §3) is one thread: `exchange_replay --publish GROUP:PORT`
processes a request, sends that request's market updates on the group, then calls the anchor,
`fillscope_request_anchor(request_number, updates_published)` (the back end's
`exchange/matcher/lockstep_anchor.h`), where a debugger stops. A stop freezes only the replay, so the
viewer, a process of its own listening to the group, keeps painting the books after that request.
Each Continue advances them by exactly one request. A replayed run is the same every time it is
stepped.

**The producer and the viewer are separate processes**, for the same reason: a stop freezes the
process it is in. Before the back end, the producer was a stand-in frame source, which sent each
frame and then called an anchor of its own; the rules below were learned on it, and the request
anchor follows them. The stand-in was retired in step 8 of the plan ([PLAN.md](PLAN.md)).

**Two ways to stop at the anchor**, both binding on the first run, with nothing to arm:
- **Self-stop:** when the process is traced, the anchor executes a breakpoint instruction (`int3`
  on x86-64); untraced, it does nothing. How it works, and why it is written as it is, are in
  [ARCHITECTURE.md](ARCHITECTURE.md), §6.
- **Breakpoint by name**, set from the launch configuration: `break fillscope_request_anchor` in GDB,
  `breakpoint set -n fillscope_request_anchor` in LLDB.

## Rules for the anchor
1. **Its body must survive optimization.** GCC merges identical functions at `-O2` (`-fipa-icf`,
   enabled at `-O2` on GCC 16.2.1); an empty anchor could be merged with every other empty
   function, and a breakpoint on it would fire from unrelated code. An inlined anchor would execute
   its self-stop inside its caller. Today neither can happen: the one caller is in another file,
   and the build doesn't use link-time optimization. The measures keep that true if either changes:
   the anchor is `noinline`, its store has a reader (`lastAnchoredRequest`),
   an `asm volatile` memory clobber keeps the store, and the self-stop's `asm` takes the arguments
   as inputs, so a debugger can read them in an optimized build too. Checked by the
   `ReplayLockstep.AtO2` tests, which run the replay
   built at `-O2` (without the sanitizers, whose own code changes what an optimized build can show a
   debugger) under GDB and LLDB.
2. **Its first statement must be plain code of our own source.** GDB puts a breakpoint by name
   on the first statement; LLDB puts it after the anchor's stores, at the `if`. When that was an
   `std::atomic` store, which libstdc++ inlines even at `-O0`, LLDB's breakpoint opened
   `atomic_base.h` (GDB's stayed in our file). When it was an
   `std::optional` assignment, GDB's breakpoint in the sanitizer build stopped before the argument
   was stored, and every stop read the frame source's `frame_index` as 0. So the anchor stores to
   plain variables. Guarded by the `ReplayLockstep` tests, which run in both builds, and a mutant
   of the atomic version.
3. **The code under study builds at `-O0`**, the default build type. At higher levels, line-table
   entries are dropped, and a breakpoint can bind and never fire.

## What is confirmed
On 2026-10-23, with GCC 16.2.1, GDB 17.2 and LLDB 22.1.8, by the `replay_debugger_tests` on every
full test run:
- both stop methods stop inside the request anchor, in `lockstep_anchor.cpp`, with the request's
  number and the updates published readable, under GDB and LLDB, at `-O0` and at `-O2`: GDB's
  breakpoint by name at the anchor's first statement, LLDB's at its `if`, and a self-stop at the
  breakpoint instruction;
- at the stop after request N, a viewer listening to the group has every update the replay
  published through request N: the LLDB tests' driver (`tests/backend/replay_lldb_driver.py`) is the
  viewer, and counts them at each stop;
- self-stop does nothing without a tracer (`LockstepAnchor.SelfStopWithoutATracerDoesNotStop`, in
  the back end's tests).

And by hand: the viewer, live, followed a replay of 502 requests publishing on the exchange's
stream, every one of its 505 updates, with no recovery.

On 2026-10-19, with the frame source, and the viewer as it was then (it read the frame source's
frames); a record, since both were retired in step 8:
- the viewer keeps painting at 60 frames a second while the producer is stopped, through pauses of
  several minutes;
- each Continue adds one frame to the viewer, and Step Over from a stop returns to the frame
  source's loop;
- both stop methods stop inside the anchor, in `anchor.cpp` (GDB's breakpoint by name at its first
  statement, LLDB's at its `if`, a self-stop at the breakpoint instruction), with
  `frame_index` readable, under GDB and LLDB, at `-O0` and at `-O2`, as its debugger tests
  checked on every full test run;
- at each stop for frame N, the viewer already has the run's description and frames 0 to N: the
  LLDB tests' driver acted as the viewer and counted what had arrived at each stop;
- self-stop does nothing without a tracer (`Anchor.SelfStopWithoutATracerDoesNotStop`), and stops a
  traced process in the anchor (`Anchor.SelfStopStopsATracedProcessInTheAnchor`, in which the test
  itself is the tracer).

If the viewer and a producer ever share a process, a stop would freeze the viewer too. GDB's
`scheduler-locking` default, `replay` (confirmed on GDB 17.2), lets other threads run during a step;
whether that gives the UI enough time to paint is untested.

## Processes, threads and deterministic replay
**A book is matched on one thread, from one totally ordered input stream.** Orders from many
clients arrive in a nondeterministic order; the exchange sequences them, and from then on matching
is deterministic. So the two needs can coexist:
- **Live mode:** the exchange, a trading client and the UI as separate processes, over TCP (order
  entry) and UDP multicast (market data).
- **Replay and backtest mode:** the recorded sequenced stream fed back in; the same stream gives the
  same result, checked by a checksum. Latency in a backtest is simulated, not network latency.

The process split, the threads in each, and whether backtesting runs over sockets are open
decisions for the owner.

## Publishing the debugger's state
An assistant working beside the debugger can't see the debug session: a stop gives it no location,
no stack and no values. So a VS Code extension, `tools/vscode-debug-state/`, writes
`.fillscope/debug-state.json` on every change, using the Debug Adapter Protocol's `stackTrace`,
`scopes` and `variables` requests. A wrong file means an explanation of a program state that never
existed, so it meets these requirements, each checked by its tests (and each shown, by a mutant, to
fail a test when broken):
- **One entry per debug session.** A lockstep run debugs two programs at once; the viewer's
  "running" must not overwrite the replay's stop. A child session names its parent.
- **What the program is doing, exactly:** "starting" until the launch or attach succeeds;
  "running"; "stopped", with the stack and locals; "moving" after an accepted `goto` or
  `restartFrame`, which reposition the program without running it (a stop follows); "exited", with
  the exit code; "terminated" when debugging has ended, which the protocol says does not mean the
  program has exited; "ended" when the session is over.
- **"Not fetched" is distinct from "fetched, and empty":** `variables: null` with
  `variablesError`, versus an empty list.
- **Resuming publishes too:** "running" on a `continued` event *and* when the debugger accepts a
  resuming request from VS Code (continue, next, the steps, reverse continue, restart), which
  debuggers are not expected to announce. A refused request leaves the stop published, since the
  program never moved; a request accepted after a newer stop arrived doesn't hide that stop;
  another thread resuming alone (by its event, its request, or a continue the debugger says
  resumed only some threads) doesn't clear the stopped thread's stop, even while that stop is
  still being read. A stop that names no thread shows the first thread's stack and says which
  (`framesThreadId`).
- **Out-of-date stops are dropped:** a stop whose details arrive after the program has moved on
  (resumed, stopped again, or restarted under the same session id) is not published, and no more
  requests are made for it.
- **An ended session stays ended,** and nothing from a session's earlier incarnation (an old debug
  adapter's last messages or exit, after a restart that reused the session's id) changes the
  current one. A session ends when VS Code stops it, when VS Code reports it terminated, or, for a
  debug adapter VS Code runs as a process of its own, when that process exits: then a reason is
  given unless it exited with code 0. (CodeLLDB runs inside VS Code, so no exit is reported for
  it.)
- **A shortened stack, or list of locals, says so.** The frame count is published only when it is
  exact: a reply with fewer frames than were asked for is the whole stack (whatever the
  debugger's total, a hint that may overstate, says). At most ten structured locals have their
  members read; the locals are the scope the debugger marks as such, else the one named Local or
  Locals, and expensive scopes (registers) are never read. So a stop costs at most a dozen or so
  requests, which queue ahead of VS Code's own. A stop's `text` and `description` are published
  separately, as the protocol defines them.
- **The file says who wrote it and when:** the writer (its process id, start time, and whether it
  is still running, which is false in the last write VS Code lets the extension make as it shuts
  it down; after a crash of VS Code's extension host only the process id can tell), a sequence
  number that increases with every write by one writer, and a timestamp; each session entry
  carries those of its last change. Writes go to a temporary file in the same directory and are
  renamed over the old one (checked by watching the calls, and by a reader on another thread that
  never sees part of a file). A failed write leaves no temporary file and removes the old file, so
  a reader finds no file rather than a wrong one.

It is activated by any debug session, since VS Code extensions are installed per user, but writes
only for a session in a workspace folder that contains its own source (a session with no folder of
its own, as from a multi-root workspace's own launch configuration, goes to the first such folder);
each such folder has its own file, and two folders that reach one directory share it. Open each Fillscope folder in one VS Code window only: two windows would overwrite each
other's file (the writer's process id tells them apart). Its JavaScript is type-checked by the
TypeScript compiler in the tests; it runs from its source, so there is no compiled copy to fall
out of date.
