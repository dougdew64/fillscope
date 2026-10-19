# Debugger lockstep: what HRW learned, and what Fillscope will test

**Purpose:** how HRW stepped a real algorithm in the VS Code debugger while its animation advanced
in step, what that cost, and what Fillscope expects to carry over or simplify.
**Status:** reference (the HRW study, 2026-10-19). Claims about Fillscope marked **verify** are
untested until the experiment at the end has run. **Read when:** designing the animation, the
frame interface between the core and the UI, the process split, or the debugger integration.

The predecessor is [HRW](https://github.com/dougdew64/rumoca/tree/hrw/hrw), an observatory for the
Rumoca Modelica compiler (Rust and egui, Windows). Its write-up is in
[architecture.md](https://github.com/dougdew64/rumoca/blob/hrw/hrw/docs/architecture.md) (§7,
"Algorithm animation steppers" and "Live trace debugging on Windows") and its
[debugger bridge README](https://github.com/dougdew64/rumoca/blob/hrw/hrw/vscode-extension/README.md).

## How HRW does it
**Three threads during a live session.** The UI thread paints every ~16 ms. A worker thread runs
compiles, because a compile takes hundreds of milliseconds; it exists for responsiveness, not for
the debugger. Each Debug click spawns an **algorithm thread** that runs the real algorithm on copies
of the data (one case runs inside the worker's real compile instead).

**The mechanism**
([live_trace.rs](https://github.com/dougdew64/rumoca/blob/hrw/crates/rumoca-phase-structural/src/live_trace.rs)):
1. **Frames over a channel.** The algorithm pushes each step's state into a channel; the UI drains
   it every paint. Recorded playback and live stepping are the same frames from two sources.
2. **A sleep after each push.** A debugger stop freezes every thread, the UI's included, so the
   algorithm thread sleeps after pushing (150 ms with a breakpoint armed, 20 ms without) to give
   the UI time to paint the frame.
3. **An anchor function.** After the sleep the thread calls `live_trace_breakpoint(index)`, which
   exists only to carry a breakpoint. Each Continue (F5) advances one frame.
4. **All-threads stepping.** F10/F11 must let the UI thread run during a step, or the animation
   goes stale. The Visual Studio debugger does this by default; LLDB needed aliases
   (`thread step-over -m all-threads`).

**Arming the breakpoint from the app** needs a VS Code extension, because an application cannot
reach VS Code's debug API. HRW writes `breakpoint-request.json`; the extension calls
`vscode.debug.addBreakpoints()` and writes `breakpoint-ack.json`. The handshake grew four layers:
- **a pre-warm at startup** (arm and remove once), because the debugger's first resolution of a
  source file took over a second;
- **waiting for the acknowledgement** before spawning the algorithm thread;
- **a 500 ms sleep**, because adding a breakpoint is asynchronous;
- **a sentinel stop** before the first frame, so no step is missed.

**The extension also publishes the debugger's state.** On every stop it writes the stack and the
innermost frame's locals to `debug-state.json` (DAP `stackTrace`, `scopes`, `variables`), with a
sequence number and timestamp so a stale file can't be mistaken for the current stop. Without it,
an assistant working beside the debugger can't see where the program is.

## What went wrong, and why
| Failure | Cause | Lesson |
|---|---|---|
| The breakpoint fired inside an unrelated library | The anchor's only statement was a store nothing read; the optimizer deleted it, and the linker merged the now-empty function with every other empty one | The anchor's body must survive optimization: give the store a reader, and make it opaque to the optimizer |
| A breakpoint showed as valid and never fired | Optimized builds drop line-table entries | Build the code under study without optimization |
| A phase breakpoint fired only once per model | The compiler cached results, so the phase didn't run again | When a breakpoint doesn't fire, first prove the code ran |
| The app died after a long pause | The GPU device didn't survive the debugger's pause | Long pauses are the normal case here; test them |
| The app said "armed" when nothing was | The acknowledgement was a boolean meaning "request read" | An acknowledgement must report the state it claims, not receipt |
| The second Debug click didn't stop | Removing a breakpoint stopped the debugger re-binding that location later in the session | Leave the anchor armed between runs |

## What Fillscope expects
**The same shape.** The matching engine pushes one frame per event (an order added, a fill, a
cancel); the UI animates frames; an anchor carries the breakpoint.

**Hypotheses, and what the experiment found** (2026-10-19, GCC 16.2.1, GDB 17.2, LLDB, from the
command line; VS Code itself is still **verify**):
- **A separate UI process removes the sleep window: confirmed.** A breakpoint freezes only the
  process being debugged. With the engine stopped for four seconds, the viewer kept painting at 60
  frames a second, and each Continue delivered exactly one more frame. No sleep, and no
  all-threads stepping.
- **The app can stop itself, removing the arming handshake: confirmed, with `int3`.** When a
  debugger is attached (checked through `TracerPid` in `/proc/self/status`), the anchor executes a
  breakpoint instruction (`asm volatile("int3")` on x86-64, what C++26's `std::breakpoint()` does).
  GDB and LLDB both stop inside the anchor with `frame_index` readable, and Continue passes it.
  Without a debugger nothing happens. **`raise(SIGTRAP)` was rejected:** under GDB it stops two
  frames down inside libc.
- **A breakpoint set by name at launch also works,** under GDB (`break fillscope_frame_anchor`) and
  LLDB (`breakpoint set -n`): it binds on the first run, so no pre-warm is needed. Whether VS Code
  shows a breakpoint set this way is **verify**.
- **GDB lets other threads run during a step,** which matters if the UI and the engine share a
  process. The `scheduler-locking` default is `replay` (confirmed on GDB 17.2), which GDB documents
  as letting other threads run during normal execution; whether that gives the UI enough time to
  paint is untested.

**Carried over unchanged:**
- **The anchor must not be empty:** GCC merges identical functions at `-O2` (`-fipa-icf`, enabled
  at `-O2` on GCC 16.2.1), so the
  anchor needs `noinline` plus a body the optimizer cannot remove.
- **The code under study builds at `-O0`.**
- **The extension is still needed** to publish the stack and locals for the assistant.

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

## The experiment
[experiments/lockstep/](../experiments/lockstep/README.md): an engine process sending frames over a
Unix socket to a Dear ImGui viewer process, built at `-O0`. The first two questions are answered
above from the command line. Still open, in VS Code with CodeLLDB:
- Do both stop methods behave as they do on the command line?
- Does stepping with F10 from the anchor advance the animation when `send` runs?
- Does the viewer survive a pause of several minutes?

In the `-O2` build the anchor keeps its own address and its store (checked with `nm` and
`objdump`).
