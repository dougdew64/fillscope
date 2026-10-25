# Architecture

**Purpose:** how Fillscope's code works: its processes, what flows between them, each part's job,
and the path one market data update and one click take through the code. **Status:** reference;
kept true to the code (a change that moves structure or data flow updates this in the same
commit). **Read when:** learning the code, or before changing how its parts connect.

This document grows from questions. When the owner asks how or why the code does something, the
answer is written here, where the next reader will look, and not left in a conversation. It is
meant to be read, not searched: each answer is worked into the explanation, rewriting or
reordering it as needed, and a part that needs more depth gets a document of its own, linked
from here. [CODE-MAP.md](CODE-MAP.md) lists every part of the code, where it is explained here,
and what the owner has asked about it, so what is not yet explained shows.

Facts owned elsewhere are linked, not repeated: the debugger's rules in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md), commands and tests in [RUNNING.md](RUNNING.md), why
things were decided in [DECISIONS.md](DECISIONS.md), the back end's own changes and known issues in
its README (`backend/README.md`).

Today the viewer shows the live order books of the back end, a fork of the trading system in
Ghosh's book ([PLAN.md](PLAN.md)), rebuilt from the exchange's market data, and lets you point at
and follow orders, and a debugger steps the exchange's matcher through a recorded run, one request at
a time, while the viewer follows.

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

**Three programs, two roles.** The exchange and the trading client are the book's; the viewer is
Fillscope's. The exchange is the market: its matcher keeps the order books, its order server
takes orders over TCP, and its publisher multicasts every change to the books. The trading client
is a trader: it sends orders and cancels and reads its fills, and it also reads the market data
and keeps its own copy of the books, which its strategies trade on. Run as
`trading_main 5 RANDOM`, it is client 5 sending random orders and cancels, which is what gives
the exchange something to do. The viewer looks like a client, since it reads the same market data
and rebuilds the same books, but it only watches. It never sends an order, and the exchange
doesn't know it is there. Without a client, the exchange's books stay empty. Without the viewer,
the market runs unseen. A replay (§3) needs no client: the recording plays back the requests the
clients sent.

**How the exchange's threads hand each other work.** Each part of the exchange runs on a thread of
its own: the order server reads the clients' requests, the matcher processes them, and the
publisher sends the market data. Between each pair is a queue (`common/lf_queue.h`): a ring of
262,144 slots, allocated at start, with one thread writing and one reading. The writer fills the
next slot and moves on, the reader takes the oldest and moves on, and all they share is an atomic
count of the slots in use, so neither ever waits on a lock; that is what *lock-free* means here.
A ring has a fixed number of slots, so a reader a whole ring behind leaves the writer nowhere to
write. The book's queue wrote anyway, over the oldest message not yet read, and nothing noticed:
the publisher numbers updates as it sends them, so an overwritten update leaves no gap in the
sequence numbers, and every listener's books would have gone wrong silently. The fork's queue
stops the program instead ("LFQueue full"), for one comparison per write. Making the writer wait
for room was the other choice: it changes the latency the book measures, and it can hang, since a
matcher waiting for room in a full queue of responses, while the order server, the only reader of
that queue, waits for room in a full queue of requests, waits forever.

**Why the first orders take about half a minute.** The back end's own code is slow to start, in
two ways. In a run of the debug build started from VS Code (its logs' times):

| from start | what |
|---|---|
| 0 to 2 s | the exchange loads, under the debugger |
| 2 to 4 s | the matcher builds its books: each ticker's pool of 1,048,576 orders (`ME_MAX_ORDER_IDS`), every one constructed in advance |
| 4 to 8.5 s | the publisher's snapshot synthesizer builds its own pool of a million updates, and clears a table of a million entries per ticker |
| 9.6 s | the order server opens port 12345; the "wait for the exchange" task sees it and starts the client |
| 11.5 to 16 s | the client builds its own books and pools the same way |
| 16 to 17 s | the client joins the market data |
| 17 to 28 s | the client sleeps 10 seconds before trading (`usleep(10 * 1000 * 1000)` in `trading_main.cpp`) |
| 28 s | its first order reaches the exchange, which publishes it, and the viewer shows it |

The pools are built up front so that trading never allocates memory, which is slow and takes an
unpredictable time; the book's design pays that cost once, at start. The checks inside them, and
in the queues (`ASSERT`, in `common/macros.h`), are a macro, so a check's message is built only
when it fails. The book's `ASSERT` was a function taking a `std::string`, which built the message
on every call, and that allocated: one allocation per message read from a queue, three per order
taken from a pool and returned. The debug build is
unoptimized, so building them takes seconds. The viewer is ready from the start and waits on
nothing: it shows the first order as soon as the exchange publishes it.

**Why separate processes:** a debugger stop freezes the whole process it is debugging. With the
viewer in its own process, it keeps painting while you step through the exchange, so the books on
screen and the debugger stay in step ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)). The back end
is already several processes talking over sockets, as trading systems do (charter decision 7).

**Why the viewer listens to multicast:** an exchange publishes its market data to whoever listens,
by multicast: one datagram reaches every member of a group. So the viewer reads exactly what the
trading clients read, needs nothing from the back end that a client doesn't, and changes nothing
about it. Everything here runs on one machine, so the exchange sends on the loopback interface,
and every listener, the viewer included, joins the groups there. Nothing leaves the machine, and it
works with no network at all. Both sides must agree: a listener joined on another interface
receives nothing. (The book's sockets ignored the interface they were given, so the exchange sent on
the one the system routes the groups to, on a machine with Wi-Fi onto the local network, and with
no network couldn't send at all. The fork fixed it.)

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
| `backend/` | The trading system Fillscope observes: a fork of the one in Ghosh's book, a git submodule ([PLAN.md](PLAN.md)) | its own build file; programs `exchange_main`, `trading_main`, `exchange_replay` |
| `src/feed/` | The exchange's market data as Fillscope reads it: decoding, receiving, and the books it describes | library `fillscope_feed` |
| `src/viewer/` | The viewer: its core (no Dear ImGui) and its drawing | library `fillscope_viewer_core`, program `viewer` |
| `src/answer/` | The answer tool: an answer's scenario played through the back end's replay, its claims checked, and the answer written for the viewer (§10) | library `fillscope_answer`, program `fillscope-answer` |
| `src/common/` | Helpers the others share: `Fd`, a file descriptor's owner; `parse_number`, strict parsing of a number from text; `write_atomically`, replacing a file so a reader never sees part of it | library `fillscope_common`, and headers |
| `tools/vscode-debug-state/` | The VS Code extension that publishes the debugger's state | JavaScript, run by VS Code |
| `tools/attribution/` | The report of who wrote each file's lines ([AI-USE.md](AI-USE.md)), Fillscope's and, with upstream's lines too, the back end's (a copy) | Python script |
| `tools/code_map/` | The check that [CODE-MAP.md](CODE-MAP.md) puts every code file in one part | Python script |
| `tests/` | One directory per area, mirroring the above | test programs per area, each in one cost tier |

Dependencies point one way: the viewer and the answer tool depend on the feed, and all three use
`src/common/`. Nothing in Fillscope links the back end: it is read only through its files, its market
data among them. The answer tool runs one of its programs, `exchange_replay`, and the tests run
them all.

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
end name the last incremental update the snapshot includes. The two streams go to two multicast
groups, incremental updates to 233.252.14.3:20001 and snapshots to 233.252.14.1:20000. The
synthesizer keeps its own copy of every book, built from the incremental updates the matcher
publishes, and sends that copy. The book's exchange sends a snapshot every 60 seconds; the exchange
started from VS Code sends one every 5 (`--snapshot-interval-seconds`).

Changes alone are enough to keep a book current, and they are small and sent as they happen. But a
listener that starts late, or misses a datagram (multicast never resends one), can't rebuild a book
from changes alone, since it lacks what came before. A snapshot lets it start over.

**A ticker** is one instrument the exchange trades, as a stock symbol names a company's shares.
The book's exchange trades 8 of them (`ME_MAX_TICKERS`), each with its own order book. They have no
names, only numbers from 0 to 7.

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

The market data doesn't say which request caused which update, or what each client was told. So
`exchange_replay --steps FILE` also writes, for each request, the numbers of the updates it made
and the responses the matcher sent to clients for it: accepted, filled (each side of a trade is
told), canceled, cancel rejected. The format is in the back end's `exchange/matcher/replay.h`;
the back end's tests check the running matcher's own updates and responses are the same. It is
what an answer steps through, a request at a time ([FEATURES.md](FEATURES.md), item 15).

**Lockstep on a replay** ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)): after each request,
`exchange_replay` calls the back end's lockstep anchor, `fillscope_request_anchor(request_number,
updates_published)`, where a debugger stops; with `--publish GROUP:PORT`, it first sends that
request's updates on a multicast group, as the exchange's publisher would. The replay is
one thread, so when the debugger stops after request N, the viewer, listening to the group, already
shows the books after it; the live exchange's publisher is a thread of its own, which a stop would
freeze mid-way. How the anchor stops a program is §6.

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

The screen shows one ticker's book at a time; the ticker buttons, 0 to 7, choose which, and hovering
over one shows how many orders its book holds.

**The context bar shows the context's three parts**, one line each, indented under the heading
"Context, as published for the assistant:" so they read as its contents. The first, always there, is
the **background**: where the books come from and how current they are ("exchange live at update
5012", or recovering and why, or a replay and where in it), which book is on screen ("showing
ticker 0"), and any recoveries or market data error. The other two lines appear only when you
have pointed at something (the **point**) or followed an order (the **follow**). The background is
there because neither of the others means anything without it: an order's place in its queue is
true as of one update, and only while the books are live or exactly replayed. Showing the ticker
lets a question like "why is the best bid empty?" be about the book you are looking at.

The background's update number can trail the status line below it, "market data: live, the books
are as of update N", by up to a tenth of a second. The status line is drawn from the books
themselves; the bar is drawn from the file, which is written at most once per 100 ms while only
market data is arriving (*Publishing*, below).

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
not resting, the last update applied to it, which is how it left if that was a cancel, and if it
wasn't, the recovery it left during (its updates were never seen one by one, so how it left isn't
known); or never seen. **A level** carries its
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
context **written successfully**, never from the live selection. The bar takes three steps. On
each successful write, the publisher turns the context into the bar's lines, as text
(`bar_text`, in `context.cpp`, gives a `ContextBarText`: the background, and the point and follow
if there are any), and keeps them (`published_bar()`). `ViewerApp::paint` hands them to the
drawing in `ViewerView`, and `draw_context_bar`, in `viewer_view.cpp`, the first thing
`draw_viewer` draws, puts them on screen with a clear button beside the point and the follow. The
text is made apart from the drawing so tests can check what the bar says without drawing it. If a write fails, the bar keeps
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

## 6. The lockstep anchor: how the debugger stops once per request
**The problem:** to step through a run one request at a time, a debugger must stop the replay once
per request. Each stop must come after that request's market data has been sent, be in our own code
rather than a library's, and have the request's number readable.

`fillscope_request_anchor(request_number, updates_published)`, in
`backend/Chapter12/exchange/matcher/lockstep_anchor.cpp`, is where it stops. `exchange_replay` calls
it after each request, once that request's updates have gone out on the group (§3), so at the stop
for request N a viewer listening has every update up to N's. The launch configuration picks one of
two ways to stop there:
- **Self-stop:** the program stops itself. Nobody sets a breakpoint; the program *is* the
  breakpoint.
- **Breakpoint by name:** the debugger sets a breakpoint on the function, as on any other.

(The anchor was first written for a stand-in, a frame source that made up frames for the viewer
before the back end arrived; the stand-in and its anchor are gone, and their lessons are in this
one.)

### Self-stop, step by step
Under a debugger with self-stop, execution halts at this line of `lockstep_anchor.cpp`, and the
debugger reports the stop as `signal SIGTRAP`:
```cpp
asm volatile("int3\n\tnop" ::"r"(request_number), "r"(updates_published));
```
How it gets there:
1. The program is started as `exchange_replay RECORDING --publish GROUP:PORT --self-stop`, which
   passes the option on with `Exchange::setSelfStop(true)`.
2. For each request, the replay hands it to the matcher, sends the updates it made, then calls
   `fillscope_request_anchor(request_number, sent)`.
3. The anchor records the request's number. Self-stop is on, so it asks whether a debugger is
   attached: `tracerAttachedNow()` reads the `TracerPid` line of `/proc/self/status`. A nonzero
   value is the process id of another process tracing this one, such as LLDB or GDB.
4. If it's traced, the anchor executes `int3`.

**What `int3` does:** it is x86's breakpoint instruction, the same one a debugger writes into
your code when you set a breakpoint. The CPU traps, and the Linux kernel turns the trap into a
`SIGTRAP` signal. Because a debugger is tracing the process (through `ptrace`), the kernel doesn't
deliver the signal. It stops the process and tells the debugger, which reports the stop. VS Code
then highlights the line.

**Why it is written this way:**
- **The tracer check keeps it safe without a debugger.** An unhandled `SIGTRAP` ends a program,
  so untraced, the anchor does nothing, and `exchange_replay --self-stop` run from a terminal just
  runs. If the status file can't be read, it says so once on stderr and doesn't stop. Linux
  reports *that* a process is traced, not by what, so under a tracer that isn't a debugger
  (`strace`, `rr`) the `SIGTRAP` ends the process: self-stop is for running under a debugger only.
  Only a line that *starts* with `TracerPid:` counts, since the status file's first line is the
  program's name, which can be any text.
- **The `nop` keeps the highlight on that line.** After `int3`, the program counter points at the
  next instruction, and that is where the debugger reports the stop. Without the `nop`, the next
  instruction would belong to whatever code follows, on another line.
- **Continue needs nothing special.** The program counter is already past `int3`, so Continue
  resumes at the `nop`, and the debugger doesn't pass the `SIGTRAP` on. The anchor returns, the
  replay takes the next request, sends its updates and stops again: one request per Continue. Step
  Over from the stop returns to the replay's loop.
- **The arguments as inputs keep them readable at the stop.** `"r"(request_number)` and
  `"r"(updates_published)` make the instruction take both in registers, so the values exist right
  there, and a debugger can show them even in an optimized build, where they might otherwise be
  gone (the tests check this at -O2).
- **Not `raise(SIGTRAP)` or C++26's `std::breakpoint()`.** Under GDB, `raise(SIGTRAP)` stopped two
  frames down, inside libc. libstdc++ implements `std::breakpoint()` out of line, so it too would
  stop inside the library. Either way the debugger would open the library's source, not ours.
- **x86 only, for now.** Another architecture needs its own breakpoint instruction and its own
  answer to how its debuggers resume past it; until then the code doesn't compile there.

### Breakpoint by name
Without `--self-stop`, the anchor never executes `int3`. Instead the debugger is told to set a
breakpoint on the function: `breakpoint set -n fillscope_request_anchor` in LLDB, `break
fillscope_request_anchor` in GDB. A breakpoint by name stops after the function's prologue, and the
debuggers place it differently: GDB 17.2 stops at the first statement, the store of
`request_number` (line 88); LLDB 22.1.8 stops after the stores, at the `if` (line 91). Step Over
from there steps through the rest of the anchor before it returns to the replay's loop.

### What keeps both working
The anchor is shaped so that both ways stop in `lockstep_anchor.cpp`, with its arguments readable,
at every optimization level:
- It stores to plain variables rather than an `std::atomic`, whose `store()` is inlined from a
  libstdc++ header even at -O0: LLDB's breakpoint by name once stopped in `atomic_base.h`.
- `[[gnu::noinline]]` keeps it a function of its own, so the `int3` executes here and not inlined
  into the replay's loop; stores no other function makes, which have a reader
  (`lastAnchoredRequest`), keep GCC from merging it with another function or removing its body.

These rules, and the tests that guard them (`replay_debugger_tests`, under GDB and LLDB, at -O0 and
-O2), are in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).

## 7. One update, end to end
1. A client's order crosses a resting sell at 101: the exchange's matcher partly fills the resting
   order and sends a trade and a modify to its market data publisher.
2. The publisher numbers each (say 812 and 813) and sends them on the incremental stream,
   possibly in one datagram.
3. On its next paint, the viewer's `FeedSocket::receive` takes the datagram, and `decode_updates`
   turns its bytes into two `MarketUpdate`s.
4. `BookBuilder::on_incremental` keeps 812 (a trade) as the ticker's last trade, applies 813 (the
   modify: the resting order's quantity) to the ticker's `Book`, adds an `OrderEvent` to the order's
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

## 9. The tests
The tests must do four things, and the machinery around them exists for the last three: test
every behaviour that matters; run, after a change, every test the change could affect (and,
between edits, little else); be trusted when they pass, which means each must be able to fail; and
give the same answer however many runs, debugger sessions and exchanges share the machine. Most of
the tests are written by an AI, and the charter trusts AI output only through checks independent
of the AI ([AI-USE.md](AI-USE.md)), so the tests are themselves checked, mechanically. Commands,
tiers and timings are in [RUNNING.md](RUNNING.md); this chapter is how and why it works.

### What is tested, and where
Each area's tests are in the matching `tests/` directory. The feed's unit tests check decoding, the
books and recovery against hand-made updates (`feed_tests`); its system test runs the real exchange
and a client and checks Fillscope's books against the exchange's snapshots, and a late reader's
recovery (`feed_system_tests`), and that the exchange's recording of that run replays to the very
updates that arrived. The viewer's core has unit tests that need no window (`viewer_tests`); the UI
tests run the whole loop above with real clicks through the Dear ImGui Test Engine, with no window
or GPU, including reading real multicast streams and moving through a replay (`viewer_ui_tests`).
Lockstep on a replay is tested under GDB and LLDB (`replay_debugger_tests`). The back end's own
tests are in the fork (`backend/Chapter12/tests/`): `lowlatency_tests`, the matching, the market
data, recording and replay, the order server, the queue, its sockets; and
`lowlatency_allocation_tests`, what must not allocate. The test machinery is tested too
(`scripts_tests`, below).

### The pieces of a test run
Several tools stack up, each handing over to the next:
- **A test** is a C++ function written with GoogleTest: `TEST(LFQueue, WritingToAFullQueueStopsTheProgram) { … }`.
  The macro also defines a global variable whose initializer registers the test, before `main`
  runs, so a program's tests are whichever test files are linked into it. `main` calls
  `RUN_ALL_TESTS()`, which runs them and returns 0 if all passed, 1 otherwise: the program's exit
  status. (UI tests, the extension's Node tests and the Python tests have their own frameworks, and
  end the same way, in an exit status.)
- **A test program** has a source (`backend/Chapter12/tests/lf_queue_test.cpp` and others), a
  recipe in a `CMakeLists.txt` (`add_executable(lowlatency_tests …)`), and the program the build
  makes (`build/debug/backend/Chapter12/tests/lowlatency_tests`, and its sanitizer copy in
  `build/asan`). The build directories aren't in the repository; git ignores them.
- **CTest** runs tests by name, and to it a test is only a named command that passes by exiting
  with 0. Fillscope's GoogleTest programs are registered one CTest test per GoogleTest test (CTest
  test `FeedSocket.ReceivesWhatIsSentToItsGroup` runs `feed_tests` with
  `--gtest_filter=FeedSocket.ReceivesWhatIsSentToItsGroup`); the back end's own tests are one CTest
  test for the whole program, since they share one matching engine that takes seconds to make. The
  other test programs, such as the Python system test, are one CTest test each.
- **A tier**, given to each program where it is registered, is its cost class: `unit`, `ui`,
  `check`, `debugger` or `system`. It sets each test's timeout (from 2 seconds for a unit test to
  120 for a system test), so a test that grows slow fails as slow the day it does, and it decides
  when the program runs: the fast tiers between edits, all of them before a commit.
- **The manifest**, `build/debug/test-manifest.txt`, written by the build, lists every test program:
  its name, its tier, its binary, and the files it uses at run time.

### Three times the tests run
- **Between edits, `iterate.py`** runs only the fast programs a change affects, in seconds. What
  "affects" means comes from the build, not a hand-kept list: each program has a *fingerprint*, a
  hash of its binary's contents, of the files it uses at run time (for a file of 8 MiB or more, such
  as Node.js's binary, its size and time instead), of the shared libraries it loads (by path, size
  and time), of the `FILLSCOPE_` environment variables, and of how CTest runs the program's tests
  (each test's command, environment, timeout and working directory, as `ctest --show-only=json-v1`
  lists them). That last is all a `CMakeLists.txt` decides that the binary doesn't already show, so
  a build file counts only through it: a new timeout reruns that tier's programs, while a comment,
  or a compile option that changes no binary (`-Werror`), reruns nothing. The record of
  passing runs, `build/debug/last-pass.json`, holds each program's fingerprint as of its last
  passing run; a program whose fingerprint differs is affected, and runs. A fingerprint is taken
  before the tests run, so a file edited meanwhile counts as changed, and recorded only if the whole
  run passes; a program whose tests were all skipped (a debugger not installed) isn't recorded, since
  it proved nothing. A change the build doesn't carry into a program, such as a comment, can't
  change its result, and doesn't make it run.
- **Before a commit, `run-tests.sh`** runs every test, in two builds: the normal debug build, and one
  with AddressSanitizer and UndefinedBehaviorSanitizer, which find bugs no assertion would (a read
  of memory never initialized found the fork's uninitialized table). It also checks that every
  CTest test belongs to a program in the manifest and every program has a test, and that every
  program in the sanitizer build is built with both sanitizers. Every build here fails on a warning
  in our own code, as CI's does. It takes about two minutes; nothing is skipped, since quick tests
  aren't worth the risk of skipping (the owner's ruling, DECISIONS.md).
- **In CI**, on every push to main and every pull request, in a clean Fedora container with only the
  commit: `run-tests.sh`, with a missing debugger failing its tests rather than skipping them,
  then the mutation check, every mutant, and the selection check. Each commit on main gets a run of
  its own. CI is the backstop, not the gate: a change is done when these pass locally, and CI,
  checked once afterwards, adds the one thing a local run can't, exactly the committed tree in a
  clean environment.

### Tests that use the network
A market data stream is only a multicast group and a port, and a TCP order port is only a number,
so two programs using the same ones meet: a test once listening on the book's own stream passed on
the updates of a replay stepped in VS Code, never checking the exchange it had started. So every
test process takes ports of its own: it claims a numbered slot by locking a file
(`fillscope-test-ports/slot-N` in the temporary directory, held until the process exits, even if it
crashes), and slot N has ports 22000 + 10 N to 22000 + 10 N + 9, clear of the book's. The exchange
and the trading client take options for where they meet (`--order-port`, `--snapshot`,
`--incremental`), which the system tests pass, and a sender on the book's stream runs throughout the
back end's system test to show none of its updates arrive. Everything goes over the loopback
interface, so nothing leaves the machine.

### Whether the tests can be trusted
A passing test says something only if it would fail on the bug it guards against. Two checks ask
that of the tests and of the machinery that chooses them, and the machinery's own code is tested
like the code it tests.

**The mutation check** (`tests/scripts/mutants.py`) plants deliberate bugs, *mutants*, one at a
time, and requires a named test to fail on each. A mutant is four things: a name, saying the bug
("a full queue is overwritten"); the file it edits; its edits, each an old text and its
replacement; and the tests that must catch it. The tests are named in one of four forms: a CTest
name or regular expression (`FeedSocket.ReceivesWhatIsSentToItsGroup`, `A|B` for either), a Test
Engine test (`ui:clicking_an_order_points_at_it`), a Node test (`node:…`), or GoogleTest tests inside
the back end's one-test program (`lowlatency:LFQueue.WritingToAFullQueueStopsTheProgram`, run with
`--gtest_filter`, since CTest has no name for anything inside it).
- *Where a mutant goes.* By text, not line number: each edit's old text must occur exactly once in
  its file. The mutant survives edits elsewhere in the file, and when its own code changes it fails
  as a problem, rather than mutating nothing or the wrong place.
- *A run.* The check keeps a copy of the working tree with a build of its own, in
  `build/mutants/tree`, and brings it up to date first, rewriting only the files that differ (with
  the time now, so the build rebuilds them), so its build is incremental and the files being edited
  are never touched. It builds the real code, then the *baseline*: every set of tests a mutant to
  run names must pass on the real code, so a test already failing can't make a mutant look caught.
  Then, for each mutant: apply its edits, rebuild, run its tests, and count it caught only if tests
  ran and one failed (a misspelt name matches nothing, and must not pass as caught); restore the
  file, whatever happened.
- *Which mutants run.* A mutant's result depends on four things: the check's own code, the mutant's
  definition, the contents of the file it mutates, and the test programs it runs, by their
  fingerprints as `iterate.py` takes them, in the copy's build. Their hash is the mutant's *key*;
  when a mutant is caught its key is recorded, in `build/mutants/last-caught.json`, and next time a
  mutant whose key is unchanged is skipped, as already known. A mutant not caught loses its record,
  so it runs again. The comparison is of what the build produced, not of where the change was: a
  change to `common/macros.h` changes no mutant's file, but every back-end program's binary, so
  every back-end mutant runs, as it must, since the queue's full check is an `ASSERT`. A change to
  the viewer's drawing code changes only `viewer_ui_tests`, and runs only the 18 of 192
  mutants that run it. `--all` runs every mutant; CI keeps no record, so it always does.
- *A mutant's life.* A mutant is born with its test, as that test's proof, or from a bug once fixed,
  to keep it found, but only for a rule whose breaking could go unnoticed: a display that says
  something false, data lost, a wrong book, test machinery that answers wrongly. A rule anyone
  sees broken the first time they look, such as where a line is drawn or how it is worded, gets a
  test but no mutant; showing once, by hand, that the test fails without the change is enough.
  Every mutant runs on each change its program sees, for as long as it exists, so this is what
  keeps them from piling up. It changes when its code or test does, since the check forces that; and it is
  deleted when the claim it guards no longer exists (76 had been by 2026-10-24, most when the
  stand-ins retired with the code they tested).

**The selection check** (`tests/scripts/selection_check.py`) chooses nothing itself: it tests the
choices `iterate.py` and the mutation check make, since a test wrongly left out says nothing. It
makes known edits in a scratch copy and requires the right choice for each, no more and no fewer: for 26, the programs `iterate.py` reports
affected, what it runs and what it then records; for 8, the mutants the mutation check would run,
with every mutant first recorded as caught. It works out the right mutants its own way (from CTest's
listing of every test, where the mutation check asks CTest about each name), so a mistake in one
doesn't hide in the other. Among its cases: a change to nothing any program uses runs none, a test
changed runs every mutant its program's tests may catch, one mutant's definition changed runs that
mutant alone (and those of the scripts' own tests), and a mutant no longer recorded runs alone.

**The machinery's own tests and mutants.** `scripts_tests` tests the parts whose mistakes would
pass silently: reading the manifest, what is never recorded as passed, the fingerprints (a test
binary's changed bytes are a change, even with its size and time kept), the record, each part of a
mutant's key, the record of caught mutants, and the copy's update. The mutation check holds mutants
of that code too ("a mutant's key leaves out the programs it runs", "the copy keeps a rewritten
file's old time"), which those tests must catch.

### Why a mutation check, and what it costs
Review by the AI that wrote a test doesn't answer whether it can fail: the blind spot that wrote
it reads it as right. A mutant answers it mechanically. It has earned its place: mutants showed that
several tests written on 2026-10-19 didn't test what they claimed, and both of CI's failures in its
first 65 runs were mutants the tests didn't catch.

These mutants aren't the thousands a mutation-testing tool generates. Each is written by hand
for one claim a test makes, paired with the bug the test says it guards against: "a full queue
stops the program" with "a full queue is overwritten". So there are about two hundred, and each
is worth running. Even so the full check costs the most of anything here, since every mutant is a
rebuild and a test run: 23 minutes for 192 (2026-10-24). The cost is lopsided:
Fillscope's own mutants take under a minute of test time between them; the rest is rebuilds and
the back end's tests, which mostly wait (the book's code sleeps after starting each thread, and its
client waits ten seconds before trading) rather than compute. Two changes brought it down: each
back-end mutant runs only the GoogleTest tests that catch it, rather than the whole program (39
minutes to 23), and a mutant is skipped when nothing it depends on has changed, so a change pays
for the mutants it can affect: after a change to the viewer's drawing code, 18 mutants in about
40 seconds; with nothing changed, under 2 seconds.

The cost grows with the mutants and with the time their tests take, both of which grow with the
code, so a very large codebase can't run every mutant on every change. The practice published for
that scale is incremental, as here: mutate only the lines a change touches, and show the reviewer
the mutants that survive (Petrović and Ivanković, "State of Mutation Testing at Google", ICSE SEIP
2018). Here the choice is by what the build produced, which is more cautious than by lines, and the
full check still runs in CI.

### One change, end to end
You change how the viewer draws an order, in `src/viewer/viewer_view.cpp`. Between edits,
`iterate.py` rebuilds and runs `viewer_ui_tests`, the one program whose binary changed, in seconds.
Before committing, `run-tests.sh` runs everything in both builds, about two minutes. The mutation
check, run locally, runs the 18 mutants of the programs the change reached. After the push, CI
runs everything again from a clean container, every mutant included, while work goes on.

## 10. Answers (`src/answer/`)
**The problem:** teach how an exchange works, maker, taker, slicer, by showing it happen, without
the explanation ever being able to show something the exchange wouldn't do. You ask a question in
the editor's chat; the assistant answers with a *scenario*: a short list of client requests, what to
say about each, and the facts it claims each one leads to. The answer tool plays those requests
through the back end's real matching engine and checks every claim against what the engine did;
the viewer shows the result a step at a time, the explanation in a pane beside the book. It is
HRW's Answers, rebuilt for an exchange ([FEATURES.md](FEATURES.md), item 15).

**Nothing goes through the running exchange or the network.** The requests are written as a
recording and replayed by `exchange_replay`, the back end's fresh matching engine with no clients,
no order server and no sockets, as any recording is (§3, *Replaying*). Its market data goes to a
file, byte for byte what the exchange would have published, and the viewer reads that file, as
`viewer --replay` does, so an answer can be stepped back and forth rather than watched go by.

**The rows are the engine's, never the assistant's.** A scenario contains requests, and only
requests: it can't say what the book looks like or what traded. Those come from replaying the
requests through Ghosh's matcher, as `exchange_replay` replays any recording (§3, *Replaying*). So
if the assistant misunderstands matching, the rows contradict its explanation rather than
illustrate it. And the facts it states in words are claims, each checked the same way: a false one
stops the answer from being written at all. This is the charter's rule that AI output is trusted
only through a check that doesn't depend on the AI.

### A scenario
Written by the assistant as JSON; this one answers "what happens when a taker trades with a
maker?":

```json
{
  "question": "What happens when a taker trades with a maker?",
  "ticker": 0,
  "clients": { "1": "the maker", "3": "the taker" },
  "intro": "A maker posts orders that wait in the book; a taker sends orders that trade with them at once.",
  "steps": [
    { "request": { "client": 1, "new": { "id": 10, "side": "buy", "price": 99, "qty": 10 } },
      "say": "The maker offers to buy 10 at 99. Nothing can trade with it, so it rests in the book.",
      "claims": [ { "bestBid": { "price": 99, "qty": 10 } }, { "trades": [] } ] },
    { "request": { "client": 1, "new": { "id": 11, "side": "sell", "price": 101, "qty": 10 } },
      "say": "And to sell 10 at 101. The gap between 99 and 101 is the spread.",
      "claims": [ { "bestAsk": { "price": 101, "qty": 10 } } ] },
    { "request": { "client": 3, "new": { "id": 30, "side": "buy", "price": 101, "qty": 4 } },
      "say": "The taker buys 4, priced at 101 so it crosses the maker's sell, and trades at once.",
      "claims": [
        { "trades": [ { "price": 101, "qty": 4, "aggressor": "buy" } ] },
        { "told": { "client": 1, "filled": { "id": 11, "price": 101, "qty": 4, "leaves": 6 } } },
        { "bestAsk": { "price": 101, "qty": 6 } } ] }
  ]
}
```

- **`clients`** gives each client number a role, so the answer says "the maker", not "client 1".
  Every client a request or a claim names must have one.
- **`request`** is a `new` order (the client's own id for it, side, price, quantity) or a `cancel`
  of one of the client's ids. Each new order needs an id of its own: the exchange keys a client's
  orders by id, and a second order under one would overwrite the first's entry.
- **`say`** is the explanation of the step.
- **`ask`**, if given, is a question the pane shows before the step is played, its `say` only
  after: an answer as a quiz on what the exchange will do, whose right answer is the replay's.
- **`claims`** are the facts the step states, five kinds: `bestBid` or `bestAsk` (the best price on
  that side, and the total resting there; `null` for an empty side); `trades` (exactly the step's
  trades, in order, so a trade left out is as false as one made up); `resting` (a client's order
  rests with this quantity, or with `null`, doesn't rest); and `told` (a client was sent a response
  about one of its orders: `accepted`, `filled`, `canceled` or `cancelRejected`, with any of its
  price, quantity and quantity left, each checked if given).

**What a scenario can't say** is refused when it is read, in the domain's words, rather than played
as something else. Ghosh's exchange has only limit orders: every order has a price, and whatever
doesn't trade rests in the book; there is no market order, and no immediate-or-cancel. So a taker
is a limit order priced to cross, and a scenario with a `market` request is refused, saying so. A
client can't modify an order either, only cancel it and send a new one, and a `modify` is refused
the same way. The matcher's own limits (256 clients, 8 tickers, a client's order ids below
1,048,576) are refused too, before the back end ever sees them; so is any field the format doesn't
have, so that a misspelt claim (`bestbid`) is never silently left unchecked.

### Making an answer
`fillscope-answer SCENARIO [--answer FILE]` (by default `.fillscope/answer.json`):

1. **Reads the scenario** (`read_scenario`, `scenario.h`), refusing it as above.
2. **Writes its requests as a recording**, `.fillscope/answer/scenario.rec`, in the format the
   exchange records (`write_recording`, `wire.h`).
3. **Replays it**: runs the back end's `exchange_replay` on it, with `--steps`, in
   `.fillscope/answer/`, where the replay writes its log too. The market data goes to
   `market.dat`; what each request did (the updates it made, and the responses the matcher sent to
   clients) to `steps.dat` (§3, *Replaying*).
4. **Reads both back** (`read_market_data_file`, and `read_steps_file` in `wire.h`), and checks
   the replay is the scenario's: one step per request, each step's request the scenario's request,
   byte for byte, and the steps' updates following on from 1 to the last.
5. **Checks every claim** (`check_scenario`, `answer.h`), below.
6. **If all hold, writes the answer** atomically (`write_atomically`, as the context is written).
   If any is false it prints each, what was claimed and what is true, and leaves the answer file as
   it was: a false answer is never shown.

It exits 0 when it has written the answer, 1 when it refused, and 2 for a bad command line. A run
takes about 1.3 seconds, nearly all of it the replay building its matching engine.

**The answer file** holds the question, the roles, and each step: its request in words ("The
taker sends order 30: buy 4 at 101."), its question and what it says, the numbers of the updates
it made, the responses in words, and its claims as sentences, each with `holds`. It names the
market data and steps files the answer rests on, with a fingerprint of each, so the viewer can
tell a current answer from one whose replay has since changed. And it carries the scenario as
written: the viewer reads it with the same `read_scenario`, and checks the claims itself, rather
than take the file's word for it.

### How a claim is checked
`check_scenario` follows the replay step by step, as the viewer will: a fresh `BookBuilder` is
given each step's updates in turn, so after step N the books are exactly the replay's after
request N. A claim about step N is checked against four things (`StepState`, `check.h`):

- **The book after the step**, the scenario's ticker: `bestBid`, `bestAsk` and `resting` read it.
- **The step's own updates**: the trades among them, for `trades`. A trade's side in the market
  data is that of the incoming order that crossed (the matcher's `match`), so it is the aggressor.
- **The step's own responses**, for `told`, which holds if any response to that client, about that
  order, has the claimed kind and every number given.
- **Which exchange id each client's order has.** A scenario names an order by its client and the
  client's id for it; the book knows it by the exchange's id. The accepted responses pair the two,
  so `resting` can find the order: "the exchange never accepted that order" if no response did.

Each claim is also turned into the sentence the pane will show: "The best ask is 101, for 6.", "This
step trades 4 at 101 (an incoming buy crossing).", "The maker is told order 11 filled 4 at 101, with
6 left." A false one says what is true instead: "the best ask is 101, for 6".

**Why the bytes are read here, not by including the back end's headers:** as with the market data
(§3), Fillscope writes the recording and reads the steps from their layouts (`wire.h`), a second,
independent reading of the formats; the back end's tests pin them down from its side. A mismatch
between the two shows as a step that isn't the scenario's request, which the tool refuses.

**What tests it:** `answer_tests` (the unit tier): every refusal of a scenario, the bytes of a
request, a recording and a steps file (field by field, against the back end's layout), each kind of
claim true and false, and a replay that isn't the scenario's. `answer_system_tests` (the system
tier, since each run starts a replay, about 800 MB): the tool run whole on real replays, which also
pins down how Ghosh's exchange matches, as an answer will state it: an order filled at the resting
order's price, the incoming order told first; a buy sweeping two prices, best first; what an order
can't fill resting in the book; a cancel, and a cancel of an order no longer there rejected with no
update; and a false claim refused with the earlier answer left as it was.

### The viewer playing an answer
`viewer --answer FILE` (in VS Code, the **viewer (answers)** launch) is a viewer for answers, not
the live exchange: it watches the answer file, and shows each answer written to it, from its
start, within a quarter of a second (`ViewerApp::poll_answer`). It sees a new answer by the file's
inode, size and time (`stamp_of`), all of which the tool's rename over the old file changes.

**Loading one** (`load_answer`, `answer_state.h`) reads the answer file, and refuses it, saying why,
if it isn't one, if its scenario can't be read, or if a replay file it names is gone or no longer
has the fingerprint the answer recorded: a replay run over those files since would make every step
describe something else, so such an answer is *stale*. Otherwise it reads the market data and the
steps, and checks every claim again with `check_scenario`, the tool's own checker: a claim is shown
true or false as this viewer finds it against the replay, never as the file says. It also notes
whose each order is, from the responses that accepted them, which the market data doesn't say: the
book gets a column, `whose` ("the maker's 11"), while an answer is shown, so the exchange's order
ids in the book and the clients' ids in the pane can be matched.

**The frames.** An answer is stepped a frame at a time: its introduction, with the books empty;
then, for each step, its question if it asks one, with the books still as before the step, and the
step played, with the books after it (`answer_frames`). The books move only with the frames: the
answer's market data is shown as a replay (`open_replay`), and each frame asks for the books after
one update (`ViewerApp::answer_to`, then `replay_to`); the replay's own controls aren't drawn, so the
pane and the books can't disagree.

**The pane**, on the left of the window, with the market on the right, shows the question, the
controls, and the frame: the introduction; a step's request and its question; or a step played, with
what it says, the updates it made, what each client was told, and each claim marked true or
**FALSE** (with what is true instead). When no answer can be shown, the pane says why, nothing of
the last answer stays on screen, and the status line and the context bar say no answer is shown,
not that the viewer is waiting for the exchange.

**The context names where you are**: `background.answer` gives the answer file, its question, the
step (0 for the introduction), how many steps it has, and whether the step's question is shown and
the step not yet played; `background.noAnswer` says why none is shown. So a question asked in the
chat at a step is about that step, and the assistant can rebuild exactly what you see.

**Pictures of it.** `--answer-frame N` opens an answer at frame N, and `--screenshot FILE`, with
`--exit-after`, writes the window as the viewer exits (a PPM, read back from the frame just drawn).
They let the assistant see what it built, which it otherwise can't, and suit demos.

**What tests it:** `viewer_tests`: the frames, loading (claims checked again, whose each order is,
and every refusal: no answer, not an answer, stale, a replay file gone, a scenario that can't be
read), a replaced file's new stamp, the context and the bar, and the options. `viewer_ui_tests`,
with real clicks on answers written by hand (`tests/support/answer_fixture.h`): waiting for an
answer; stepping through one, forward and back, the books moving with the frames, a step's question
before its explanation, and the context's step; a new answer replacing the shown one from its
start; a claim the viewer finds false marked so; and a stale answer refused, nothing of the last
left on screen.

## 11. A reading order for the code
1. `src/feed/market_update.h`: what the exchange sends.
2. `src/feed/book.h`, then `book_builder.h`: a book, and keeping every book in step.
3. `src/viewer/main.cpp`, then `viewer_app.cpp`: the viewer's setup, and its loop.
4. `src/viewer/viewer_state.h`, `context.h`, `context_publisher.h`: what the loop works on.
5. `src/viewer/viewer_view.cpp`: drawing and clicks.
6. `src/feed/market_update.cpp`, `feed_socket.cpp` and `market_data_file.cpp`: the bytes, the socket and
   a replay's file underneath.
7. `src/answer/scenario.h`, `check.h`, then `answer.cpp`: an answer's scenario, its claims, and
   making one.
