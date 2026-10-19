# Architecture

**Purpose:** how Fillscope's code works: its processes, what flows between them, each part's job,
and the path one frame and one click take through the code. **Status:** reference; kept true to
the code (a change that moves structure or data flow updates this in the same commit). **Read
when:** learning the code, or before changing how its parts connect.

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

**Why files for the assistant:** the assistant runs in another program and can't see inside the
viewer or the debugger. Each writes what it knows to a file the assistant reads. Neither waits to
be asked: you ask your question in the editor, which the viewer never sees, so the viewer publishes
the moment anything changes.

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
- `SourceHello { seed, frame_count }` opens every connection. It describes the run, and its seed is
  what makes a frame reproducible: the same seed always gives the same frames.
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
check both payloads are trivially copyable.

**The socket** (`net.h`) is a Unix `SOCK_SEQPACKET` socket in Linux's abstract namespace:
- *SEQPACKET* keeps message boundaries (one send arrives as one datagram) and delivers in order,
  so there is no framing code to write.
- *Abstract namespace* means a name with no file on disk, so nothing to clean up after a crash.
- `Fd` owns a file descriptor and closes it in its destructor (RAII); it is move-only, so a
  descriptor has exactly one owner.
- `listen_on`, `connect_to` and `send_message` return `std::expected<..., std::error_code>`: the
  result, or the operating system's reason it failed, which the caller reports as it is.
- `MessageReceiver::drain` is the viewer's side. It never blocks: it accepts a sender if none is
  connected (and says so, in its result: a new sender means a new connection), then takes the
  messages waiting, up to a bound, and returns. A sender that sends garbage is dropped and
  `error()` says why.

## 4. The frame source (`tools/frame_source/`)
**The problem:** the viewer and the debugger integration need frames before the real system
exists.

`run_frame_source` connects to the viewer (retrying while the viewer starts), sends a
`SourceHello`, then for each frame:
```cpp
const Frame frame = generator.next();       // the next frame, from the seed
send_message(*viewer, frame);               // to the viewer (its failure is reported, not shown here)
fillscope_frame_anchor(frame.index);        // where a debugger stops
std::this_thread::sleep_for(options.interval);
```
`FrameGenerator` draws from `std::mt19937_64` seeded with the run's seed, in a fixed order (price
step, qty, id), so a seed fully determines every frame. That is what a replay address relies on,
and it holds on any compiler: the standard fixes `std::mt19937_64`'s output exactly, and the
project's own `draw_uniform` turns raw draws into bounded values, where
`std::uniform_int_distribution`'s method would vary from one standard library to another. A test
pins seed 42's first frames to values computed independently.

## 5. The anchor (`src/lockstep/`)
`fillscope_frame_anchor(frame_index)` is where a debugger stops after each frame, either by a
breakpoint set on it by name or by the anchor stopping itself when the process is traced. Its
rules (a body that survives optimization, a first statement in our own source) and their evidence
are in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).

## 6. The viewer (`src/viewer/`)
**The problem:** show what a source sends, let you point at and follow things, and publish exactly
what you've assembled for the assistant, without ever claiming more than it has been given.

### The loop
Dear ImGui is an immediate-mode UI: no tree of widget objects persists between paints. Every paint
the code calls the widget functions again, and a widget function returns `true` on the paint it is
clicked. So the viewer is one loop, once per paint (`main.cpp`):

1. **Take in messages:** `receiver.drain(...)`, then `state.apply(message)` for each. A
   `SourceHello` starts a new run; when it does, `selection.new_run()` drops the point, which
   belonged to the old run.
2. **Draw, and collect clicks:** `draw_viewer(state, selection, publisher.published(),
   publisher.error())` draws everything and returns `ViewerActions`: what you clicked, if anything.
   It changes nothing itself.
3. **Apply the clicks:** `apply(actions, selection)`.
4. **Publish:** `publisher.update(make_context(state, selection), actions.any(), now)`.

Because drawing only reads state and returns actions, a test can draw the viewer with any state
and check what it shows and what a click returns, without the rest of the program.

### The pieces
| Type (file) | Holds | Why it is shaped this way |
|---|---|---|
| `Run` (`viewer_state.h`) | One run's description, frames, and prices as floats for the plot | A class, so the frames and prices can only change together through `append` and can't fall out of step |
| `ViewerState` (`viewer_state.h`) | The current run, whether the source is connected, timing | Replaced run by run: frame indices restart at 0 in every run, so frames of two runs never mix |
| `Selection` (`context.h`) | What is pointed at and what is followed, each with a sequence number | Both numbers come from one counter, so whichever is higher is what you did last |
| `Context` (`context.h`) | One value: everything published at one moment | Built by `make_context`; a plain value with `==`, so "has anything changed?" is one comparison |
| `ContextPublisher` (`context_publisher.h`) | The file, what it last wrote successfully, the last error | Keeps the *published* context separately from the live one; see below |

**Point and follow are kept apart** because they ask different things: a point is the subject of a
question ("why didn't *this* fill?"), a follow is a lens on it (this id, across every frame).
`make_context` builds the follow section with a ranges pipeline over the run's frames: keep the
frames with the followed id, take their indices. It lists up to 1,000 and always counts all.

**A replay address** is the run's seed plus a frame's index. Running the frame source with that
seed reproduces that frame exactly. A run whose source didn't describe itself has no seed, so its
frames have no replay address, and the file says why.

### Publishing, and why the bar can't lie
`ContextPublisher::update` is called every paint:
- If the context equals what was last published, nothing is written.
- **After a click** it writes at once, so the file is current by the time you can ask about it.
- **Otherwise** (frames arriving) it writes at most once per 100 ms of real time; a change held
  back is written by a later paint, so the file always catches up.
- A write goes to a temporary file, then `rename`s it over the old one: a reader sees the whole
  old file or the whole new one, never half.

The context bar is drawn from `publisher.published()`, the last context **written successfully**,
never from the live selection. If a write fails, the bar keeps showing what the file actually
holds and adds the error in red. So the bar never claims the assistant has something it doesn't.

**One consequence you can see:** a click is published in the same paint, at the end of the loop,
and the bar shows it on the next paint, about 16 ms later. Frames arriving reach the bar within
100 ms.

## 7. The debug-state extension (`tools/vscode-debug-state/`)
VS Code's debugger talks to GDB or LLDB through the Debug Adapter Protocol. The extension listens
to that conversation for every debug session and, at each stop, asks for the stack and the
innermost frame's locals, then writes `.fillscope/debug-state.json`. Its requirements, such as one
entry per session and treating a step request as "running", are in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md). `debug_state.js` holds the logic and doesn't use VS
Code, so its tests drive it with fake sessions; `extension.js` connects it to VS Code.

## 8. One frame, end to end
1. `FrameGenerator::next()` in the frame source makes frame 12 from the seed.
2. `send_message` encodes it (`encode`: kind 2, padding, 24 bytes) and sends one datagram.
3. The frame source calls `fillscope_frame_anchor(12)`; under a debugger it stops there, and the
   extension writes the stopped state.
4. On its next paint, the viewer's `drain` receives the datagram, `decode`s it into a `Message`
   holding a `Frame`, and `ViewerState::apply` appends it to the run.
5. `draw_viewer` shows it in the table and the plot; `make_context` counts it in the background,
   and in the follow if its id is followed.
6. Within 100 ms, `ContextPublisher` writes the new context, and the bar shows it on the next paint.

## 9. One click, end to end
1. You click frame 12's row. That paint, `ImGui::Selectable` for the row returns `true`, and
   `draw_frame_table` sets `actions.point_at = frame`.
2. `draw_viewer` returns the actions; `apply` calls `selection.point_at(frame)`, which takes the
   next sequence number.
3. `make_context` builds a `Context` whose point holds frame 12 and its replay address.
4. `publisher.update(..., user_action = true, ...)` writes the file at once, through a temporary
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
3. `src/viewer/main.cpp`: the viewer's loop, top to bottom.
4. `src/viewer/viewer_state.h`, `context.h`, `context_publisher.h`: what the loop works on.
5. `src/viewer/viewer_view.cpp`: drawing and clicks.
6. `src/channel/net.cpp` and `message.cpp`: the socket and the bytes underneath.
