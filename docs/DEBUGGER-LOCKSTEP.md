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
- **Self-stop:** when the process is traced (a nonzero `TracerPid` line in `/proc/self/status`),
  the anchor executes a breakpoint instruction (`asm volatile("int3")` on x86-64). Untraced, it
  does nothing. Linux reports a tracer, not what kind of tool it is, so under a tracer that isn't a
  debugger (`strace`, `rr`) the instruction's SIGTRAP ends the process: self-stop is for running
  under a debugger only. Rejected: `raise(SIGTRAP)`, which under GDB stops two frames down inside
  libc; and C++26's `std::breakpoint()`, which libstdc++ implements out of line, so it too would
  stop inside the library.
- **Breakpoint by name**, set from the launch configuration: `break fillscope_frame_anchor` in GDB,
  `breakpoint set -n fillscope_frame_anchor` in LLDB.

## Rules for the anchor
1. **Its body must survive optimization.** GCC merges identical functions at `-O2` (`-fipa-icf`,
   enabled at `-O2` on GCC 16.2.1); an empty anchor could be merged with every other empty
   function, and a breakpoint on it would fire from unrelated code. So it is `noinline`, its store
   has a reader (`last_anchored_frame`), and an `asm volatile` memory clobber keeps the store.
   Checked in the `-O2` build with `nm` and `objdump`: the anchor keeps its own address and its
   store.
2. **Its first statement must be plain code of our own source.** A breakpoint by name lands on the
   first statement. When that was an `std::atomic` store, which libstdc++ inlines even at `-O0`,
   LLDB's breakpoint opened `atomic_base.h` (GDB's stayed in our file). When it was an
   `std::optional` assignment, GDB's breakpoint in the sanitizer build stopped before the argument
   was stored, and every stop read `frame_index` as 0. So the anchor stores to plain variables.
   Guarded by the `LockstepDebugger` tests, which run in both builds, and a mutant of the atomic
   version.
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
no stack and no values. So a VS Code extension, `tools/vscode-debug-state/`, writes
`.fillscope/debug-state.json` on every change, using the Debug Adapter Protocol's `stackTrace`,
`scopes` and `variables` requests. A wrong file means an explanation of a program state that never
existed, so it meets these requirements, each checked by its tests (and each shown, by a mutant, to
fail a test when broken):
- **One entry per debug session.** A lockstep run debugs two programs at once; the viewer's
  "running" must not overwrite the frame source's stop. A child session names its parent.
- **"Not fetched" is distinct from "fetched, and empty":** `variables: null` with
  `variablesError`, versus an empty list.
- **Resuming publishes too:** "running" on a `continued` event *and* when the debugger accepts a
  resuming request from VS Code (continue, next, step, restart...), which debuggers are not
  expected to announce. A refused request leaves the stop published, since the program never
  moved; a request accepted after a newer stop arrived doesn't hide that stop; another thread
  resuming alone doesn't clear the stopped thread's stop.
- **Out-of-date stops are dropped:** a stop whose details arrive after the program has moved on
  (resumed, stopped again, or restarted under the same session id) is not published, and no more
  requests are made for it.
- **An ended session stays ended:** "exited" (with the exit code) when the program ends, "ended"
  when the session does, including when the debug adapter crashes; nothing changes a session after
  it has ended.
- **A shortened stack, or list of locals, says so.** The frame count is published only when it is
  exact; the debugger's own total is a hint that may overstate, so it is never published as exact.
  At most ten structured locals have their members read, and expensive scopes (registers) are
  never read, so a stop costs at most a dozen or so requests, which queue ahead of VS Code's own.
- **The file says who wrote it and when:** the writer (its process id, start time, and whether it
  is still running; VS Code shutting the extension down ends every session and says the writer has
  stopped), a sequence number that increases with every write by one writer, and a timestamp; each
  session entry carries those of its last change. Writes go through a temporary file and a rename,
  so a reader never sees half a write (tested with a reader on another thread), and a failed write
  leaves no temporary file.

It is activated by any debug session, since VS Code extensions are installed per user, but writes
only for a session in a workspace folder that contains its own source; each such folder has its
own file. Open each Fillscope folder in one VS Code window only: two windows would overwrite each
other's file (the writer's process id tells them apart). Its JavaScript is type-checked by the
TypeScript compiler in the tests; it runs from its source, so there is no compiled copy to fall
out of date.
