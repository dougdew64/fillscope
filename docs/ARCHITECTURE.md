# Architecture

**Purpose:** how Fillscope's code works: its processes, what flows between them, each part's job,
and the path one market data update and one click take through the code. **Status:** reference;
kept true to the code (a change that moves structure or data flow updates this in the same
commit). **Read when:** learning the code, or before changing how its parts connect.

This document grows from questions. When the owner asks how or why the code does something, the
answer is written here, where the next reader will look, and not left in a conversation.

Facts owned elsewhere are linked, not repeated: the debugger's rules in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md), commands and tests in [RUNNING.md](RUNNING.md), why
things were decided in [DECISIONS.md](DECISIONS.md), the back end's own changes and known issues in
its README (`backend/README.md`).

Today the viewer shows the live order books of the back end, a fork of the trading system in
Ghosh's book ([PLAN.md](PLAN.md)), rebuilt from the exchange's market data, and lets you point at
and follow orders, and a debugger steps the exchange's matcher through a recorded run, one request at
a time, while the viewer follows. The stand-ins that came first (a frame source, the socket it sends
on, and the debugger anchor it calls) remain only for their debugger tests, until step 8 of the plan
retires them.

## 1. The big picture
The back end's programs run, the viewer listens to the exchange, and an assistant reads what the
viewer and VS Code write:

```
 trading client ── orders, TCP 12345 ──► exchange process ──── market data, UDP multicast ────► viewer process
 (trading_main)  ◄── responses ───────── (exchange_main)        incremental updates,             rebuilds the books
                 ◄──────── market data ── matcher, order         233.252.14.3:20001               you point and follow
                                          server, publisher,     snapshots, every so often,            │
                                          snapshot synthesizer   233.252.14.1:20000                    ▼
                                                                                            .fillscope/context.json
                                                                                            (what you pointed at and
                       VS Code debug sessions ──► .fillscope/debug-state.json                 follow, as of an update)
                       (written by the debug-state extension: stacks, locals)                          │
                                                       │                                               │
                                                       └──────────► the assistant ◄────────────────────┘
                                                                    reads both files and answers
                                                                    your questions in the editor
```

**Why separate processes:** a debugger stop freezes the whole process it is debugging. With the
viewer in its own process, it keeps painting while you step through the exchange, so the books on
screen and the debugger stay in step ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)). The back end
is already several processes talking over sockets, as trading systems do (charter decision 8).

**Why the viewer listens to multicast:** an exchange publishes its market data to whoever listens,
by multicast: one datagram reaches every member of a group. So the viewer reads exactly what the
trading clients read, needs nothing from the back end that a client doesn't, and changes nothing
about it. The exchange's code names the loopback interface, but its sockets don't use the name:
it sends on the interface the system routes the groups to (on a machine with Wi-Fi, onto the local
network), and the system loops a copy back to listeners on the machine (a known issue in the back
end's README).

**The assistant** is today Claude, in Claude Code beside the debugger in VS Code: when you ask
about what you have pointed at or where a program is stopped, it reads the two files and answers.
Nothing ties the files to it: they are plain JSON in the workspace, and neither the viewer nor the
extension knows who reads them. It reads them only when you ask; it doesn't watch them.

**Why files for the assistant:** the assistant runs in another program and can't see inside the
viewer or the debugger. Each writes what it knows to a file the assistant reads. Neither waits to
be asked: you ask your question in the editor, which the viewer never sees, so the viewer publishes
whatever you do at once, and other changes within a tenth of a second (§4).

## 2. The code, by directory
| Directory | Its job | Built as |
|---|---|---|
| `backend/` | The trading system Fillscope observes: a fork of the one in Ghosh's book, a git submodule ([PLAN.md](PLAN.md)) | its own build file; programs `exchange_main`, `trading_main` |
| `src/feed/` | The exchange's market data as Fillscope reads it: decoding, receiving, and the books it describes | library `fillscope_feed` |
| `src/viewer/` | The viewer: its core (no Dear ImGui) and its drawing | library `fillscope_viewer_core`, program `viewer` |
| `tools/vscode-debug-state/` | The VS Code extension that publishes the debugger's state | JavaScript, run by VS Code |
| `src/channel/`, `src/lockstep/`, `tools/frame_source/` | The stand-ins the debugger tests still use: a frame source, its socket, and the debugger anchor (§6) | libraries `fillscope_channel`, `fillscope_lockstep`, `frame_source_lib`; program `frame_source` |
| `tools/attribution/` | The report of who wrote each file's lines ([AI-USE.md](AI-USE.md)), Fillscope's and, with upstream's lines too, the back end's (a copy) | Python script |
| `tests/` | One directory per area, mirroring the above | one test program per area and cost tier |

Dependencies point one way: the viewer depends on the feed; the feed uses the channel's `Fd` (a
file descriptor's owner); the frame source depends on the channel and the anchor. Nothing in
Fillscope links the back end: it is read only through its market data. (Tests run its programs.)

## 3. The market data (`src/feed/`)
**The problem:** rebuild every order book exactly as the exchange has it, from what the exchange
publishes, and know when that isn't possible.

**What arrives** (`market_update.h`): each update is 42 bytes, packed and little-endian: a sequence
number, a type, the exchange's order id, a ticker, a side, a price, a quantity and a priority (an
order's place in its price's queue). One datagram may hold several. `decode_updates` reads them
field by field rather than including the back end's header, so it is a second, independent reading
of the format the back end's own tests pin down; it rejects bytes that aren't whole updates of
known types.

**Two streams.** Incremental updates (add, modify, cancel, trade) are numbered 1, 2, 3, ... Every
so often the exchange's snapshot synthesizer also sends a snapshot of every book: a start, a clear
for each of the 8 tickers followed by its resting orders, an end, numbered from 0; the start and
end name the last incremental update the snapshot includes.

**A book** (`book.h`) holds one ticker's resting orders by order id, and each price's queue by
priority, bids best (highest) first and asks best (lowest) first. It checks every update against
what it holds: an add for an order it already has, a modify or cancel for one it hasn't, or a
modify at another price, means updates were missed, so it refuses the update, stays as it was, and
says why. A modify changes only the quantity: the exchange keeps an order's place when it is partly
filled. A trade changes nothing (the modify or cancel beside it does).

**Keeping the books in step** (`book_builder.h`, the protocol the book's own trading client
follows):
- **Live:** an incremental update is applied when it is the next one expected.
- **Recovering:** a gap (multicast doesn't retransmit), a first update other than 1 (the feed was
  already running), or an update a book refuses, starts recovery. Incremental updates are queued
  until a whole snapshot arrives: a start numbered 0, every update after it in sequence, an end. The
  queued updates after the snapshot's last must follow it without a gap, or the snapshot isn't
  enough and the next one is awaited. Then every book is rebuilt from the snapshot and the queued
  updates are applied.
- Snapshots are ignored while live, as the trading client ignores them.

`BookBuilder` also keeps, for every order, each update applied to it (`OrderEvent`: its number, its
type, the order's quantity after it, and whether it was found in a snapshot rather than seen
arriving), and a record of each recovery: the updates from `after_seq` to `snapshot_seq` were never
applied one by one, so an order's history has none of their events. It keeps every order's history,
about 40 bytes an event, so a long run takes memory in proportion to its length.

**Receiving** (`feed_socket.h`): a `FeedSocket` joins one stream's group on the default interface,
as the trading clients do, bound to the group's address so only that group's datagrams arrive; it
never blocks, and decodes what is waiting.

**What a replay is.** A live run of the exchange can never be repeated: the clients choose their
orders at random, the network delivers them at slightly different times, and the exchange's
threads interleave differently each time. But the matcher at its heart is deterministic: it takes
one request at a time from one queue and never reads a clock, so the same requests in the same
order always give the same decisions. A *recording* is the list of requests in the order the
matcher processed them; a *replay* feeds that list to a fresh matcher with nothing else running,
and everything the live run did happens again, identically, as a chess game's move list
reproduces every position. So any moment of a recorded run can be rebuilt exactly, as often as
wanted.

**Replaying** (`market_data_file.h`): a run of the exchange can be rebuilt exactly. The exchange
records the requests its matcher processes, in the order it processes them (`exchange_main
--record FILE`); the back end's `exchange_replay` feeds a recording through a fresh matcher, with
no clients and no network, and writes the incremental updates it publishes to a file, after a
header naming the recording. The matcher is single-threaded and reads no clock, so the same
requests in the same order give the same updates: the file is the live stream, byte for byte
(`feed_system_tests` checks it against a live run). `read_market_data_file` reads it, checking its
updates are numbered 1, 2, 3, ...

**Lockstep on a replay** ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)): with `--publish
GROUP:PORT`, `exchange_replay` sends each request's updates on a multicast group, as the exchange's
publisher would, and then calls the back end's lockstep anchor,
`fillscope_request_anchor(request_number, updates_published)`, where a debugger stops. The replay is
one thread, so when the debugger stops after request N, the viewer, listening to the group, already
shows the books after it; the live exchange's publisher is a thread of its own, which a stop would
freeze mid-way. The anchor follows the same rules as the frame source's anchor (§6).

## 4. The viewer (`src/viewer/`)
**The problem:** show the books as the market data describes them, let you point at and follow
orders, and publish exactly what you've assembled for the assistant, without ever claiming more
than it has been given.

### The loop
Dear ImGui is an immediate-mode UI: no tree of widget objects persists between paints. Every paint
the code calls the widget functions again, and a widget function returns `true` on the paint it is
clicked. So the viewer is one loop, once per paint. `ViewerApp` (`viewer_app.h`) holds it, and both
`main.cpp` and the UI tests run it, so the tests take exactly the program's steps:

1. **Take in market data** (`ViewerApp::receive`): what each stream has, up to a bound per paint,
   incremental updates first, into the `BookBuilder`. The last error receiving or decoding is kept,
   and shown.
2. **Draw, and collect clicks** (`ViewerApp::paint`): `draw_viewer(ViewerView{...})` draws
   everything from the state, the viewer's timing, and what was published, and returns
   `ViewerActions`: what you clicked, if anything. It changes nothing itself.
3. **Apply the clicks:** `apply(actions, selection, state)`.
4. **Publish:** `publisher.update(make_context(state, selection), trigger, now)`, where the trigger
   is `PublishTrigger::user_action` if you clicked anything and `background` otherwise.

`main.cpp` paces the loop itself, about 60 paints a second, and swaps without waiting for the
display (no vsync). With vsync, a swap waits for the compositor, which sends a window no frames
while it can't be seen (behind another window, on another workspace); the loop then stopped, and
with it the reading of market data, whose buffers overflowed and lost updates, and the check for
SIGINT and SIGTERM.

When the viewer exits (its window closed, `--exit-after`, or SIGINT or SIGTERM, which only ask the
loop to stop), it takes in what is waiting and `ViewerApp::finish()` publishes once more with the
writer marked as no longer running, so a reader can tell a stale file from a live one. A failure to
publish is printed on stderr when it starts and when it ends, and a failed last write makes the
viewer exit 1. If it can't join a stream, it says why and exits 1.

Because drawing only reads what it is given and returns actions, a test can draw the viewer with
any state and check what it shows and what a click returns.

### What it draws
The context bar; the market data's state (live as of an update, or recovering and why), or in a
replay, where in it the books are and its controls; a button
for each ticker; and the shown ticker's book, one row per resting order, asks above bids with each
side's best price nearest the middle, and within a price the orders in the order they will fill.
Clicking a row points at its order, clicking a price points at its level, clicking an order's id
follows it. A followed order's history is drawn below.

### The pieces
| Type (file) | Holds | Why it is shaped this way |
|---|---|---|
| `ViewerState` (`viewer_state.h`) | The `BookBuilder`, the ticker shown, the last market data error | The books are the market data's; the ticker shown is the user's view, published as background |
| `Selection` (`context.h`) | What is pointed at (an order, or a price level) and what is followed (an order), each with a sequence number | Both numbers come from one counter, so whichever is higher is what you did last |
| `Context` (`context.h`) | One value: everything published at one moment | Built by `make_context`; a plain value with `==`, so "has anything changed?" is one comparison |
| `ContextPublisher` (`context_publisher.h`) | The file, what it last wrote successfully, the last error | Keeps the *published* context separately from the live one; see below |

**Point and follow are kept apart** because they ask different things: a point is the subject of a
question ("why didn't *this* fill?"), a follow is a lens on it (this order, through every update).

**An order pointed at** carries what such a question needs: resting, its side, price, quantity and
priority, and its place in its price's queue (the orders, and their quantity, that fill before it);
not resting, the last update applied to it (how it left); or never seen. **A level** carries its
total and its orders in the order they will fill (up to 100 listed, all counted). **A followed
order** carries its events (up to 1,000 listed, all counted) and the recoveries since it was first
seen, whose updates its events may be missing. Everything is as of the last incremental update the
books include, which the context names (`asOfSeq`).

**A replay, and its addresses.** With `--replay FILE`, the viewer shows a replay's market data
instead of the live exchange: its books are built from the file's updates 1 to N, and controls move
N anywhere, forward by applying the next updates, back by rebuilding from the first
(`ViewerApp::replay_to`). The books are then exactly the recorded run's after update N, so a point
or follow carries a replay address: the market data file, the recording it came from, and N.
Applying the file's updates 1 to N rebuilds that state. Live, there is no replay address, and the
file says why and how to record a run.

### Publishing, and why the bar can't lie
`ContextPublisher::update` is called every paint:
- If the context equals what was last published, and the last write succeeded, nothing is
  written.
- **After a click** it writes at once, so the file is current by the time you can ask about it.
- **Otherwise** (market data arriving) it writes at most once per 100 ms of real time; a change held
  back is written by a later paint, so the file always catches up.
- A write goes to a temporary file, is flushed to disk, then `rename`d over the old one: a reader
  sees the whole old file or the whole new one, never half. A failed write removes its temporary
  file, and its error says why (the system's reason, not just the fact). It also removes the old
  file if it can, since that no longer says what you have assembled; then nothing is published
  until a write succeeds.
- A failed write is retried on later paints, even if nothing has changed, until one succeeds.
- The file names its writer: the program, its process id, when it started, and whether it is
  still running.

The context bar, and the book's highlights, are drawn from `publisher.published()`, the last
context **written successfully**, never from the live selection. If a write fails, the bar keeps
showing what the file actually holds and adds the error in red, and the tooltips say a click won't
reach the assistant. So nothing on screen claims the assistant has something it doesn't.

**One consequence you can see:** a click is published in the same paint, at the end of the loop,
and the bar shows it on the next paint, about 16 ms later. Market data arriving reaches the bar
within 100 ms plus a paint, if the write succeeds.

## 5. The debug-state extension (`tools/vscode-debug-state/`)
VS Code's debugger talks to GDB or LLDB through the Debug Adapter Protocol. The extension listens
to that conversation for every debug session and, at each stop, asks for the stack and the
innermost frame's locals, then writes `.fillscope/debug-state.json`. Its requirements, such as one
entry per session and treating an accepted step request as "running", are in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md). `debug_state.js` holds the logic and doesn't use VS
Code, so its tests drive it with fake sessions; `extension.js` connects it to VS Code, choosing
each session's file by its workspace folder, and its tests run it against a fake `vscode` module.

## 6. The stand-ins the debugger tests use (`src/channel/`, `tools/frame_source/`, `src/lockstep/`)
Before the back end, a frame source made up frames and sent them to the viewer, and a debugger
stopped it once per frame. The viewer no longer reads frames, and lockstep now runs on a replay
(§3), through the back end's own anchor, built to the same rules as the one below. The frame source,
its socket and its anchor remain, with their debugger tests (`lockstep_debugger_tests`), until step
8 of the plan retires them. What follows explains how such an anchor stops a program, which holds
for both.

**The channel** (`message.h`, `net.h`): a frame source sends a `SourceHello { seed, frame_count }`,
then `Frame`s, held as `std::variant<SourceHello, Frame>`, each one datagram on a Unix
`SOCK_SEQPACKET` socket in Linux's abstract namespace (message boundaries kept, nothing on disk). A
receiver acknowledges a sender it accepts with one byte, so a sender never believes frames arrived
that were only queued. `Fd` owns a file descriptor and closes it in its destructor; the feed's
socket uses it too.

**The frame source** (`frame_source.cpp`) connects, waits for the acknowledgement, describes its
run, then for each frame: makes it from its seed (`FrameGenerator`, `std::mt19937_64` with the
project's own bounded draw, so a seed gives the same frames on any compiler), sends it, and calls the
anchor. The LLDB tests' driver acts as its receiver.

### The anchor: how the debugger stops once per item
**The problem:** to step through a producer's output one item at a time, a debugger must stop it
once per item. Each stop must come after that item has been sent, be in our own code rather than a
library's, and have the item's index readable.

`fillscope_frame_anchor(frame_index)` is where it stops. The frame source calls it right after
sending each frame, so at the stop for frame N its receiver already has frames 0 to N. The launch
configuration picks one of two ways to stop there:
- **Self-stop:** the program stops itself. Nobody sets a breakpoint; the program *is* the
  breakpoint.
- **Breakpoint by name:** the debugger sets a breakpoint on the function, as on any other.

#### Self-stop, step by step
Under a debugger with self-stop, execution halts at this line of `anchor.cpp`, and the debugger
reports the stop as `signal SIGTRAP`:
```cpp
asm volatile("int3\n\tnop" ::"r"(frame_index));
```
How it gets there:
1. The program is started as `frame_source --self-stop`. The frame source passes that on with
   `set_self_stop(true)`.
2. Its loop sends a frame, then calls `fillscope_frame_anchor(frame.index)`.
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

#### Breakpoint by name
Without `--self-stop`, the anchor never executes `int3`. Instead the debugger is told to set a
breakpoint on the function: `breakpoint set -n fillscope_frame_anchor` in LLDB, `break
fillscope_frame_anchor` in GDB. A breakpoint by name stops after the function's prologue, and the
debuggers place it differently: GDB 17.2 stops at the first statement, the store of `frame_index`;
LLDB 22.1.8 stops after the stores, at the `if`. Step Over from there steps through the rest of
the anchor before it returns to the loop.

#### What keeps both working
The anchor is shaped so that both ways stop in `anchor.cpp`, with `frame_index` readable, at every
optimization level. That is why it stores to plain variables rather than an `std::atomic`, whose
`store()` is inlined from a libstdc++ header: LLDB's breakpoint by name once stopped in
`atomic_base.h`. These rules, and the tests that guard them, are in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).

## 7. One update, end to end
1. A client's order crosses a resting sell at 101: the exchange's matcher partly fills the resting
   order and sends a trade and a modify to its market data publisher.
2. The publisher numbers each (say 812 and 813) and sends them on the incremental stream,
   possibly in one datagram.
3. On its next paint, the viewer's `FeedSocket::receive` takes the datagram, and `decode_updates`
   turns its bytes into two `MarketUpdate`s.
4. `BookBuilder::on_incremental` applies 812 (a trade: the ticker's last trade) and 813 (the modify:
   the resting order's quantity) to the ticker's `Book`, adds an `OrderEvent` to the order's
   history, and expects 814 next.
5. `draw_viewer` shows the smaller quantity in the order's row; `make_context` puts the books'
   update number, 813, in the background, and the new quantity in the point if that order is
   pointed at, and in the follow's events if it is followed.
6. Within 100 ms plus a paint, `ContextPublisher` writes the new context, and the bar shows it on
   the next paint.

## 8. One click, end to end
1. You click order 17's row. That paint, `ImGui::Selectable` for the row returns `true`, and
   `draw_book` sets `actions.point_at = OrderRef{ticker, 17}`.
2. `draw_viewer` returns the actions; `apply` calls `selection.point_at(...)`, which takes the next
   sequence number.
3. `make_context` builds a `Context` whose point holds order 17 as the books have it now: resting at
   its price, with the orders and quantity ahead of it in its queue.
4. `publisher.update(..., PublishTrigger::user_action, ...)` writes the file at once, through a
   temporary file and a rename.
5. On the next paint the bar, drawn from the newly published context, shows "Pointing at order 17
   of ticker 0: ...".
6. You ask the assistant about it; it reads `.fillscope/context.json`.

## 9. Where the tests fit
Each area's tests are in the matching `tests/` directory. The feed's unit tests check decoding,
the books and recovery against hand-made updates (`feed_tests`); its system test runs the real
exchange and a client and checks Fillscope's books against the exchange's snapshots, and a late
reader's recovery (`feed_system_tests`), and that the exchange's recording of that run replays to the very updates
that arrived. The viewer's core has unit tests that need no window
(`viewer_tests`); the UI tests run the whole loop above with real clicks through the Dear ImGui Test
Engine, with no window or GPU, including reading real multicast streams and moving through a replay
(`viewer_ui_tests`). Lockstep on a replay is tested under GDB and LLDB (`replay_debugger_tests`). The back
end's own tests are in the fork. How tests are selected, tiered and run is in
[RUNNING.md](RUNNING.md).

**How the tests are checked.** Two checks ask whether the tests themselves can be trusted:
- **The mutation check** (`tests/scripts/mutants.py`). A test can pass while testing nothing: an
  assertion left out, a wrong expectation, a test that never reaches the code it is named for. So
  the check plants deliberate bugs ("mutants"), one at a time, in a scratch copy of the repository:
  for example, the matcher's `price < ask` made `price <= ask`, so a buy no longer trades at the
  ask's price. For each, it rebuilds what the edit affects, runs the tests named for that mutant,
  and requires one of them to fail; a mutant that survives is a blind spot in the tests. Each of
  the 150 is a rebuild and a test run, so the whole check takes about 15 minutes. A mutant names
  the tests expected to catch it, so a change to one test needs only the mutants that name it (the
  script runs a subset by mutant name; choosing them from what changed is a later improvement).
- **The selection check** (`tests/scripts/selection_check.py`). Between edits, `iterate.py` runs
  only the test programs an edit affects, judged from what the build produced rather than a
  hand-kept list. If it judged wrongly, a test that should run wouldn't, and the tests would look
  green over a break. So the check makes 26 known edits in a scratch copy and requires
  `iterate.py` to choose exactly the right programs for each, no more and no fewer; each edit is a
  rebuild, about 4 minutes in all. It matters when the test machinery or the build's structure
  changes (a new test program, a new dependency).

CI runs both on every push.

## 10. A reading order for the code
1. `src/feed/market_update.h`: what the exchange sends.
2. `src/feed/book.h`, then `book_builder.h`: a book, and keeping every book in step.
3. `src/viewer/main.cpp`, then `viewer_app.cpp`: the viewer's setup, and its loop.
4. `src/viewer/viewer_state.h`, `context.h`, `context_publisher.h`: what the loop works on.
5. `src/viewer/viewer_view.cpp`: drawing and clicks.
6. `src/feed/market_update.cpp`, `feed_socket.cpp` and `market_data_file.cpp`: the bytes, the socket and
   a replay's file underneath.
