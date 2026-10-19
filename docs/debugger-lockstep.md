# Debugger lockstep

**Purpose:** how a debugger steps the code that produces frames while the viewer's animation
follows, frame by frame, and the rules that keep it working. **Status:** reference. **Read when:**
designing the animation, the frame interface between the core and the UI, the process split, or
the debugger integration.

The code is in `src/lockstep/` (the anchor), `src/channel/` (the socket), `src/viewer/` and
`tools/frame_source/` (a stand-in producer); how to run it is in [RUNNING.md](RUNNING.md).

## How it works
**The producer and the viewer are separate processes.** The producer sends each frame over a Unix
socket, then calls the anchor, `fillscope_frame_anchor(frame_index)`, where a debugger stops. A
stop freezes only the process being debugged, so the viewer keeps painting from the frames it has
received. Each Continue delivers exactly one more frame; Step Over from the anchor returns to the
producer's loop, and the next frame appears in the viewer when `send` runs.

**Two ways to stop at the anchor**, both binding on the first run, with nothing to arm:
- **Self-stop:** when a debugger is attached (a nonzero `TracerPid` in `/proc/self/status`), the
  anchor executes a breakpoint instruction (`asm volatile("int3")` on x86-64, what C++26's
  `std::breakpoint()` does). Without a debugger it does nothing. `raise(SIGTRAP)` was rejected:
  under GDB it stops two frames down, inside libc.
- **Breakpoint by name**, set from the launch configuration: `break fillscope_frame_anchor` in GDB,
  `breakpoint set -n fillscope_frame_anchor` in LLDB.

## Rules for the anchor
1. **Its body must survive optimization.** GCC merges identical functions at `-O2` (`-fipa-icf`,
   enabled at `-O2` on GCC 16.2.1); an empty anchor could be merged with every other empty
   function, and a breakpoint on it would fire from unrelated code. So it is `noinline`, its store
   has a reader (`last_anchored_frame`), and an `asm volatile` memory clobber keeps the store.
   Checked in the `-O2` build with `nm` and `objdump`: the anchor keeps its own address and its
   store.
2. **Its first statement must be in our own source.** A breakpoint by name lands on the first
   statement. When that was an `std::atomic` store, which libstdc++ inlines even at `-O0`, LLDB's
   breakpoint opened `atomic_base.h` (GDB's stayed in our file). Guarded by
   `LockstepDebugger.LldbBreakpointByNameStopsInTheAnchor` and a mutant of the original code.
3. **The code under study builds at `-O0`**, the default build type. At higher levels, line-table
   entries are dropped, and a breakpoint can bind and never fire.

## What is confirmed
On 2026-10-19, with GCC 16.2.1, GDB 17.2, LLDB, and VS Code with CodeLLDB:
- the viewer keeps painting at 60 frames a second while the producer is stopped, through pauses of
  several minutes;
- each Continue adds one frame to the viewer, and Step Over behaves as described above;
- both stop methods stop inside the anchor, in `anchor.cpp`, with `frame_index` readable, under GDB
  and LLDB; the `lockstep_debugger_tests` check this on every full test run;
- self-stop does nothing without a debugger (`Anchor.SelfStopWithoutADebuggerDoesNotStop`).

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
no stack and no values. So a VS Code extension will write the stack and the innermost frame's
locals to a file on every stop ([PLAN.md](PLAN.md)), using the Debug Adapter Protocol's
`stackTrace`, `scopes` and `variables` requests. A wrong file means an explanation of a program
state that never existed, so it must meet four requirements:
- **"Not fetched" is distinct from "fetched, and empty":** no variables with a reason, versus an
  empty list.
- **Resuming publishes too:** a "running" state on continue and at session end, so the last stop
  isn't taken for the current one.
- **A shortened stack says so,** with the true frame count.
- **Every write carries a sequence number and a timestamp,** and readers check staleness before
  trusting anything else. Writes go through a temporary file and a rename, so a read never sees
  half a write.
