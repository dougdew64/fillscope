# Architecture

**Purpose:** how Fillscope's code works: its processes, what flows between them, each part's job,
and the path one frame and one click take through the code. **Status:** reference; kept true to
the code (a change that moves structure or data flow updates this in the same commit). **Read
when:** learning the code, or before changing how its parts connect.

This document grows from questions. When the owner asks how or why the code does something, the
answer is written here, where the next reader will look, and not left in a conversation.

Facts owned elsewhere are linked, not repeated: the debugger's rules in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md), commands and tests in [RUNNING.md](RUNNING.md), why
things were decided in [DECISIONS.md](DECISIONS.md).

Today Fillscope is plumbing on stand-ins: a frame source that makes up frames, and a viewer that
shows them and lets you point at and follow them. The order book, matching and backtesting will
plug into this plumbing; none of them exist yet.

## 1. The big picture
Three programs run, and an assistant reads what two of them write:

```
 frame source process                 viewer process                      VS Code
 ────────────────────                 ──────────────                      ───────
 makes frames ──── Unix socket ─────► shows frames                        debug sessions:
 calls the anchor  (messages)         you point and follow                  frame source, viewer
 after each frame                          │                                     │
                                           ▼                                     ▼
                              .fillscope/context.json         .fillscope/debug-state.json
                              (what you pointed at,           (written by the debug-state
                               what you follow)                extension: stacks, locals)
                                           │                                     │
                                           └───────────► the assistant ◄─────────┘
                                                         reads both files and answers
                                                         your questions in the editor
```

**Why separate processes:** a debugger stop freezes the whole process it is debugging. With the
viewer in its own process, it keeps painting while you step through the frame source, so the
animation and the debugger stay in step ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)). Separate
processes also mean the parts talk over sockets, as trading systems do (charter decision 8).

**The assistant** is today Claude, in Claude Code beside the debugger in VS Code: when you ask
about what you have pointed at or where the program is stopped, it reads the two files and
answers. Nothing ties the files to it: they are plain JSON in the workspace, and neither the viewer
nor the extension knows who reads them. It reads them only when you ask; it doesn't watch them.

**Why files for the assistant:** the assistant runs in another program and can't see inside the
viewer or the debugger. Each writes what it knows to a file the assistant reads. Neither waits to
be asked: you ask your question in the editor, which the viewer never sees, so the viewer publishes
whatever you do at once, and other changes within a tenth of a second (§6).

## 2. The code, by directory
| Directory | Its job | Built as |
|---|---|---|
| `src/channel/` | The messages a source sends, their encoding, and the socket that carries them | library `fillscope_channel` |
| `src/lockstep/` | The debugger anchor | library `fillscope_lockstep` |
| `src/viewer/` | The viewer: its core (no Dear ImGui) and its drawing | library `fillscope_viewer_core`, program `viewer` |
| `tools/frame_source/` | The stand-in that makes frames | library `frame_source_lib`, program `frame_source` |
| `tools/vscode-debug-state/` | The VS Code extension that publishes the debugger's state | JavaScript, run by VS Code |
| `tests/` | One directory per area, mirroring the above | one test program per area and cost tier |

Dependencies point one way: the viewer and the frame source depend on the channel, and the frame
source on the anchor; no program depends on the viewer or the frame source. (Tests may: the
viewer's UI tests link the frame source's library to know which frames to expect.)

## 3. The channel: what crosses the socket (`src/channel/`)
**The problem:** two processes must exchange typed values over a byte stream, and the receiver
must know where each value ends and what kind it is.

**Two kinds of message**, in `message.h`:
- `SourceHello { seed, frame_count }` is the first thing the frame source sends on a connection. It
  describes the run, and its seed is what makes a frame reproducible: the same seed always gives
  the same frames. (A source may send frames without one; then its frames have no replay address.)
- `Frame { index, price, qty, id }` is one frame (`frame.h`). Today every field is a stand-in;
  `id` recurs across frames so there is something to follow.

They are held in one type, `using Message = std::variant<SourceHello, Frame>;`. A variant holds
exactly one of its alternatives, and `std::visit` runs the code for whichever it holds. The
compiler rejects a visit that doesn't handle every alternative, so adding a third message kind
forces every handler to deal with it.

**On the wire:** each message is one datagram: a 4-byte kind, 4 bytes of padding, then the
payload's bytes. `encode` and `decode` convert. `decode` returns `std::expected<Message,
std::string>`: the message, or why the bytes aren't one (too short, unknown kind, wrong size).
Copying raw bytes is valid only for types whose bytes are their whole value, so `static_assert`s
check that both payloads have unique object representations: trivially copyable, and no padding.

**The socket** (`net.h`) is a Unix `SOCK_SEQPACKET` socket in Linux's abstract namespace:
- *SEQPACKET* keeps message boundaries (one send arrives as one datagram) and delivers in order,
  so there is no framing code to write.
- *Abstract namespace* means a name with no file on disk, so nothing to clean up after a crash.
- `Fd` owns a file descriptor and closes it in its destructor (RAII); it is move-only, so a
  descriptor has exactly one owner.
- `listen_on`, `connect_to` and `send_message` return `std::expected<..., std::error_code>`: the
  result, or the operating system's reason it failed, which the caller reports as it is.
- `MessageReceiver::drain` is the viewer's side. It never blocks: it accepts a sender if none is
  connected (and says so, in its result: a new sender means a new connection), tells the sender it
  was accepted, then takes the messages waiting, up to a bound, and returns. A sender that sends
  garbage is dropped and `error()` says why. While one sender is connected, any other is refused:
  accepted and closed at once.
- **Why accepting is acknowledged:** a `connect` succeeds as soon as the connection is queued,
  before the viewer has taken it, so a source that sent at once could fill the socket's buffer and
  believe its frames had arrived. So the frame source waits (`wait_until_accepted`) for the
  viewer's one-byte acknowledgement before sending anything; a refused source sees the connection
  closed instead, and says the viewer has another source.

## 4. The frame source (`tools/frame_source/`)
**The problem:** the viewer and the debugger integration need frames before the real system
exists.

`run_frame_source` connects to the viewer (retrying while the viewer starts), waits for the viewer
to accept the connection, sends a `SourceHello`, then for each frame:
```cpp
const Frame frame = generator.next();       // the next frame, from the seed
send_message(*viewer, frame);               // to the viewer (its failure is reported, not shown here)
fillscope_frame_anchor(frame.index);        // where a debugger stops
std::this_thread::sleep_for(options.interval); // between frames, not after the last
```
`FrameGenerator` draws from `std::mt19937_64` seeded with the run's seed, in a fixed order (price
step, qty, id), so a seed fully determines every frame. That is what a replay address relies on,
and it holds on any compiler: the standard fixes `std::mt19937_64`'s output exactly, and the
project's own `draw_uniform` turns raw draws into bounded values, where
`std::uniform_int_distribution`'s method would vary from one standard library to another. A test
pins seed 42's first frames to values computed independently.

## 5. The anchor: how the debugger stops once per frame (`src/lockstep/`)
**The problem:** to step through the frames one at a time, a debugger must stop the frame source
once per frame. Each stop must come after that frame has reached the viewer, be in our own code
rather than a library's, and have the frame's index readable.

`fillscope_frame_anchor(frame_index)` is where it stops. The frame source calls it right after
sending each frame (§4), so at the stop for frame N the viewer already has frames 0 to N. The
launch configuration picks one of two ways to stop there:
- **Self-stop:** the program stops itself. Nobody sets a breakpoint; the program *is* the
  breakpoint.
- **Breakpoint by name:** the debugger sets a breakpoint on the function, as on any other.

### Self-stop, step by step
Under a debugger with self-stop, execution halts at this line of `anchor.cpp`, and the debugger
reports the stop as `signal SIGTRAP`:
```cpp
asm volatile("int3\n\tnop" ::"r"(frame_index));
```
How it gets there:
1. The launch configuration starts `frame_source --self-stop`. The frame source passes that on
   with `set_self_stop(true)`.
2. Its loop sends a frame to the viewer, then calls `fillscope_frame_anchor(frame.index)`.
3. The anchor records the index. Self-stop is on, so it asks whether a debugger is attached:
   `tracer_attached_now()` reads the `TracerPid` line of `/proc/self/status`. A nonzero value is
   the process id of another process tracing this one, such as LLDB or GDB.
4. If it's traced, the anchor executes `int3`.

**What `int3` does:** it is x86's breakpoint instruction, the same one a debugger writes into
your code when you set a breakpoint. The CPU traps, and the Linux kernel turns the trap into a
`SIGTRAP` signal. Because a debugger is tracing the process (through `ptrace`), the kernel doesn't
deliver the signal. It stops the process and tells the debugger, which reports the stop. VS Code
then highlights the line.

**Why it is written this way:**
- **The tracer check keeps it safe without a debugger.** An unhandled `SIGTRAP` ends a program,
  so untraced, the anchor does nothing, and `frame_source --self-stop` run from a terminal just
  runs. If the status file can't be read, it says so once on stderr and doesn't stop. Linux
  reports *that* a process is traced, not by what, so under a tracer that isn't a debugger
  (`strace`, `rr`) the `SIGTRAP` ends the process: self-stop is for running under a debugger only.
- **The `nop` keeps the highlight on that line.** After `int3`, the program counter points at the
  next instruction, and that is where the debugger reports the stop. Without the `nop`, the next
  instruction would belong to the function's closing brace.
- **Continue needs nothing special.** The program counter is already past `int3`, so Continue
  resumes at the `nop`, and the debugger doesn't pass the `SIGTRAP` on. The anchor returns, the
  loop sleeps, sends the next frame and stops again: one frame per Continue. Step Over from the
  stop returns to the frame source's loop.
- **`"r"(frame_index)` keeps the frame's index readable at the stop.** It makes the instruction
  take the index as an input in a register, so the value exists right there, and a debugger can
  show it even in an optimized build, where it might otherwise be gone.
- **Not `raise(SIGTRAP)` or C++26's `std::breakpoint()`.** Under GDB, `raise(SIGTRAP)` stopped two
  frames down, inside libc. libstdc++ implements `std::breakpoint()` out of line, so it too would
  stop inside the library. Either way the debugger would open the library's source, not ours.
- **x86 only, for now.** Another architecture needs its own breakpoint instruction and its own
  answer to how its debuggers resume past it; until then the code doesn't compile there.

### Breakpoint by name
Without `--self-stop`, the anchor never executes `int3`. Instead the launch configuration tells
the debugger to set a breakpoint on the function: `breakpoint set -n fillscope_frame_anchor` in
LLDB, `break fillscope_frame_anchor` in GDB. A breakpoint by name stops after the function's
prologue, and the debuggers place it differently: GDB 17.2 stops at the first statement, the store
of `frame_index`; LLDB 22.1.8 stops after the stores, at the `if`. Step Over from there steps
through the rest of the anchor before it returns to the loop.

### What keeps both working
The anchor is shaped so that both ways stop in `anchor.cpp`, with `frame_index` readable, at every
optimization level. That is why it stores to plain variables rather than an `std::atomic`, whose
`store()` is inlined from a libstdc++ header: LLDB's breakpoint by name once stopped in
`atomic_base.h`. These rules, and the tests that guard them, are in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).

## 6. The viewer (`src/viewer/`)
**The problem:** show what a source sends, let you point at and follow things, and publish exactly
what you've assembled for the assistant, without ever claiming more than it has been given.

### The loop
Dear ImGui is an immediate-mode UI: no tree of widget objects persists between paints. Every paint
the code calls the widget functions again, and a widget function returns `true` on the paint it is
clicked. So the viewer is one loop, once per paint. `ViewerApp` (`viewer_app.h`) holds it, and both
`main.cpp` and the UI tests run it, so the tests take exactly the program's steps:

1. **Take in messages** (`ViewerApp::receive`): `receiver.drain(...)`. When a new source sends its
   first valid message, `begin_connection()` drops the old run and the point first, so a new
   source's frames never join an old run (and never claim its seed). A connection that sends
   nothing valid leaves the run you are examining in place. Then `apply(message)` for each; a
   `SourceHello` starts a new run (so does a first frame, when there is no run), and when one
   starts, `selection.new_run()` drops the point, which belonged to the old run. The follow stays:
   an id can mean something in the next run too.
2. **Draw, and collect clicks** (`ViewerApp::paint`): `draw_viewer(ViewerView{...})` draws
   everything from the state, the viewer's timing, and what was published, and returns
   `ViewerActions`: what you clicked, if anything. It changes nothing itself.
3. **Apply the clicks:** `apply(actions, selection)`.
4. **Publish:** `publisher.update(make_context(state, selection), trigger, now)`, where the trigger
   is `PublishTrigger::user_action` if you clicked anything and `background` otherwise.

When the viewer exits (its window closed, `--exit-after`, or SIGINT or SIGTERM, which only ask the
loop to stop), it takes in what is waiting and `ViewerApp::finish()` publishes once more with the
writer marked as no longer running, so a reader can tell a stale file from a live one. A failure to
publish is printed on stderr when it starts and when it ends, and a failed last write makes the
viewer exit 1.

Because drawing only reads what it is given and returns actions, a test can draw the viewer with
any state and check what it shows and what a click returns.

### The pieces
| Type (file) | Holds | Why it is shaped this way |
|---|---|---|
| `Run` (`viewer_state.h`) | One run's number, description, frames, each id's frame indices, the price envelope, and whether its indices count 0, 1, 2, ... | A class, so all of these can only change together through `append` and can't fall out of step |
| `PriceEnvelope` (`viewer_state.h`) | The prices, exactly, as at most 4,096 buckets, each a low and a high | Bounded however long the run: when full, neighbouring buckets merge in pairs, so no spike is lost; the plot reduces it to at most one bucket per pixel and draws every one |
| `ViewerState` (`viewer_state.h`) | The current run, whether the source is connected, why the last one was dropped | Replaced run by run: frame indices restart at 0 in every run, so frames of two runs never mix |
| `Selection` (`context.h`) | What is pointed at and what is followed, each with a sequence number | Both numbers come from one counter, so whichever is higher is what you did last |
| `Context` (`context.h`) | One value: everything published at one moment | Built by `make_context`; a plain value with `==`, so "has anything changed?" is one comparison |
| `ContextPublisher` (`context_publisher.h`) | The file, what it last wrote successfully, the last error | Keeps the *published* context separately from the live one; see below |

**Point and follow are kept apart** because they ask different things: a point is the subject of a
question ("why didn't *this* fill?"), a follow is a lens on it (this id, across every frame).
`make_context` builds the follow section from the run's index of each id's frames, kept as frames
arrive, so it costs the same however long the run. It lists up to 1,000 and always counts all.

**A replay address** is the run's seed plus a frame's index. Running the frame source with that
seed reproduces that frame exactly, but only if the run's frames arrived numbered 0, 1, 2, ...,
which the run checks. A run whose source didn't describe itself has no seed, and one whose indices
broke the sequence can't be trusted from that point; their frames have no replay address, and the
file says why.

**Each run has a number** (1 for the viewer's first), published with the context. Frame indices
restart at 0 in every run, so the table marks a row as pointed at only when the published context
is about the same run, and its point is that very frame.

### Publishing, and why the bar can't lie
`ContextPublisher::update` is called every paint:
- If the context equals what was last published, and the last write succeeded, nothing is
  written.
- **After a click** it writes at once, so the file is current by the time you can ask about it.
- **Otherwise** (frames arriving) it writes at most once per 100 ms of real time; a change held
  back is written by a later paint, so the file always catches up.
- A write goes to a temporary file, is flushed to disk, then `rename`d over the old one: a reader
  sees the whole old file or the whole new one, never half. A failed write removes its temporary
  file, and its error says why (the system's reason, not just the fact). It also removes the old
  file if it can, since that no longer says what you have assembled; then nothing is published
  until a write succeeds.
- A failed write is retried on later paints, even if nothing has changed, until one succeeds.
- The file names its writer: the program, its process id, when it started, and whether it is
  still running.

The context bar, and the table's highlights, are drawn from `publisher.published()`, the last
context **written successfully**, never from the live selection. If a write fails, the bar keeps
showing what the file actually holds and adds the error in red, and the tooltips say a click won't
reach the assistant. So nothing on screen claims the assistant has something it doesn't.

**One consequence you can see:** a click is published in the same paint, at the end of the loop,
and the bar shows it on the next paint, about 16 ms later. Frames arriving reach the bar within
100 ms plus a paint, if the write succeeds.

## 7. The debug-state extension (`tools/vscode-debug-state/`)
VS Code's debugger talks to GDB or LLDB through the Debug Adapter Protocol. The extension listens
to that conversation for every debug session and, at each stop, asks for the stack and the
innermost frame's locals, then writes `.fillscope/debug-state.json`. Its requirements, such as one
entry per session and treating an accepted step request as "running", are in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md). `debug_state.js` holds the logic and doesn't use VS
Code, so its tests drive it with fake sessions; `extension.js` connects it to VS Code, choosing
each session's file by its workspace folder, and its tests run it against a fake `vscode` module.

## 8. One frame, end to end
1. `FrameGenerator::next()` in the frame source makes frame 12 from the seed.
2. `send_message` encodes it (`encode`: kind 2, padding, 24 bytes) and sends one datagram.
3. The frame source calls `fillscope_frame_anchor(12)`; under a debugger it stops there, and the
   extension writes the stopped state.
4. On its next paint, the viewer's `drain` receives the datagram, `decode`s it into a `Message`
   holding a `Frame`, and `ViewerState::apply` appends it to the run.
5. `draw_viewer` shows it in the table and the plot; `make_context` counts it in the background,
   and in the follow if its id is followed.
6. Within 100 ms plus a paint, `ContextPublisher` writes the new context, and the bar shows it on
   the next paint.

## 9. One click, end to end
1. You click frame 12's row. That paint, `ImGui::Selectable` for the row returns `true`, and
   `draw_frame_table` sets `actions.point_at = frame`.
2. `draw_viewer` returns the actions; `apply` calls `selection.point_at(frame)`, which takes the
   next sequence number.
3. `make_context` builds a `Context` whose point holds frame 12 and its replay address.
4. `publisher.update(..., PublishTrigger::user_action, ...)` writes the file at once, through a temporary
   file and a rename.
5. On the next paint the bar, drawn from the newly published context, shows "Pointing at frame
   #12".
6. You ask the assistant about it; it reads `.fillscope/context.json`.

## 10. Where the tests fit
Each area's tests are in the matching `tests/` directory. The viewer's core has unit tests that
need no window (`viewer_tests`); the UI tests run the whole loop above with real clicks through the
Dear ImGui Test Engine, with no window or GPU (`viewer_ui_tests`). How tests are selected, tiered
and checked is in [RUNNING.md](RUNNING.md).

## 11. A reading order for the code
1. `src/channel/frame.h`, then `message.h`: the data, and the variant that carries it.
2. `tools/frame_source/frame_source.cpp`: the loop that sends it.
3. `src/viewer/main.cpp`, then `viewer_app.cpp`: the viewer's setup, and its loop.
4. `src/viewer/viewer_state.h`, `context.h`, `context_publisher.h`: what the loop works on.
5. `src/viewer/viewer_view.cpp`: drawing and clicks.
6. `src/channel/net.cpp` and `message.cpp`: the socket and the bytes underneath.
