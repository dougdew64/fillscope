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
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md), commands and tests in [RUNNING.md](RUNNING.md), how
problems with the tests and the system were tracked down in [TROUBLESHOOTING.md](TROUBLESHOOTING.md),
why things were decided in [DECISIONS.md](DECISIONS.md), the back end's own changes and known issues in
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

**A client, inside.** Each client is a process of its own, `trading_main CLIENT_ID STRATEGY`, as
`scripts/run_clients.sh` in the fork starts them, and it sees only what any client sees: the public
market data and the responses to its own orders. It runs three threads, which hand each other work
through the same lock-free queues the exchange uses (*How the exchange's threads hand each other
work*, below): the market data consumer joins the exchange's streams and queues each update; the order
gateway holds the client's TCP connection to the order server, sending its requests and receiving
its responses; and the trade engine, the only one that decides, takes each update, applies it to
the client's copy of the books, updates the features, and only then calls the strategy
(`trading/strategy/trade_engine.cpp`). The features (`feature_engine.h`) are what the strategies
trade on: the fair price, and how large the last aggressive trade was against the best quantity it
met. The strategies only read them, through getters, and compute none. Trading firms' systems are
split the same way, signals computed in one layer, an update at a time in the trading path, and
the strategies acting on them in another, so one signal serves several strategies and can be
tested and replayed without them. Fillscope's slicing detector is to be such a feature, read by a
market maker of Fillscope's own: a client of the exchange that speaks its protocols and links
nothing of the back end, as a firm's client and an exchange share only the exchange's published
protocols ([FEATURES.md](FEATURES.md), items 13 and 18).

**The book's strategies.** A trading client runs one of three: `RANDOM`, above; `MAKER`, a market
maker; or `TAKER`, a liquidity taker (`trading/strategy/` in the fork). The other two take, on the
command line, each ticker's clip (the size of every order), threshold, and risk limits, as
`scripts/run_clients.sh` there shows. Each decides only when a market update arrives, from its own
copy of the books, and keeps at most one order a side on each ticker: wanting a different price, it
cancels its order and, once the cancel is confirmed, sends a new one, if the risk check allows.
- **The market maker,** after any add, modify or cancel on a ticker with both sides quoted, quotes
  a bid and an ask of its clip there. Its fair price is the best bid and ask, each weighted by the
  quantity on the other side, so it leans toward the thinner side. It bids at the best bid if the
  fair price is at least its threshold above it, and a tick lower if not; it asks at the best ask,
  or a tick higher, the same way.
- **The liquidity taker,** after a trade on a ticker with both sides quoted, divides the trade's
  quantity by the best quantity on the side it took from. At its threshold or more, it trades its
  clip the same way, at the best price opposite, and cancels anything it has on the other side: a
  large trade is taken as a sign more will follow.

`session_system_tests` (`tests/backend/session_system_test.cpp`; named `strategy_system_tests`
until 2026-10-27, when it had grown from checking these strategies to checking a whole session)
runs both against the real exchange, with the book's settings and the random client as the flow, and checks every order each
sends against these rules, on the market as Fillscope's own books rebuild it. The recording, replayed,
says where each request came in the market data, and a client's decisions come in order, each before
the request it led to; so each request must be the rule's at some update since the one its last
request was, and before its own. Building it found three defects in the client's copy of the books,
fixed in the fork (its README lists them): the first order on an empty side didn't set that side's
best price, a clear (how a client recovering from a snapshot starts) left the old best prices and
its table of levels by price pointing at freed levels, and the book's tables started uninitialized.

**The planted slicer** (`trading/strategy/slicer.h` in the fork). A fourth strategy, Fillscope's,
there to be found: a trader working a large order by sending it in pieces, so as not to show its
size. `trading_main CLIENT SLICER --slice TICKER BUY|SELL TOTAL PIECE GAP` sends TOTAL in pieces of
PIECE, each GAP market updates on its ticker after the one before; given more than one `--slice`,
each on a ticker of its own, it works them all at once, each its own way, as one firm's execution
algorithm works several orders. Counting updates rather than
time: the market data carries no time, the viewer's axis is the update number, and a gap of updates
can be checked against the market data afterwards. Two options make it harder to see: `--gap-jitter
J` draws each gap from GAP ± J, and `--size-spread S` each piece's size from PIECE ± S, each for the
`--slice` before it. Each plan is drawn before it starts, from a generator seeded with `--slice-seed`
(the client's id by default) plus the order's place, so a run's pieces and gaps are known and the
same every time; `--slice-plan FILE` writes them, a line a piece: its ticker, quantity and gap. Each piece is priced to take liquidity, a buy at the best ask and a sell at the best
bid, or at the last piece's price if that side is empty; and each is cancelled as soon as the
exchange accepts it, as an immediate-or-cancel order would be, which this exchange doesn't have.
The matcher has done all the piece could do before the cancel reaches it, so whatever it found is
taken and the rest withdrawn; a piece that found the other side empty fills nothing. A piece's rest
once stayed, and stood above the market the slicer had pushed, where the others' orders met it
rather than rested: the market stopped, the slicer's later pieces resting and being met, which the
trades panel showed (§4, *A session, and its truth*). It sends its pieces itself,
each order's numbered 1, 2, 3, ..., rather than through the order manager, which keeps one order a side, and
checks no risk: it is to send exactly its order. The fork's tests check its plan, its options and,
through the client's own books, what it sends at each update.

**A recorded session** (`tools/sessions/record_session.py`). The background a slicer is hidden in
is the book's clients, so a session is the exchange, recording, with the market maker (client 1),
the liquidity taker (3) and the random client (5), as in the book, and the slicer (6) working an
order of each kind, each on a ticker of its own: it buys in equal pieces at regular gaps on ticker
1, sells with its gaps jittered on ticker 2, and buys in pieces of spread sizes on ticker 3. One
client rather than one for each kind: each client is a trade engine with a book for every ticker,
about 1.5 GB, and a session of six clients took 11.7 GB (measured), against 8.5 GB for four, which
leaves CI's 16 GB room for the tests beside it. Each piece is half the market maker's clip on its ticker:
the maker quotes only while both sides have orders, and pieces of its whole clip took its whole
quote, leaving the slicer's tickers one-sided after 29 to 59% of their updates, against 1 to 10%
elsewhere; at half, 12 to 34% (one session each, measured), and 12 to 15 of each order's 20 pieces
found something to take. Each ticker sees about 8 updates a second, and each order's gap is 12 of
them, so each goes in about 20 pieces over about 30 s. The script records
until the slicer has sent all three orders, stops every program and replays the recording with its
steps and its truth, and writes `session.json`: each client, its settings or its orders, each
order's plan, and whether a client recovered from a snapshot. The market's first update is the random client's first order, so the
script starts it only once the other clients are listening: on a busy machine, clients started
together joined late, two in each of three sessions (measured), and a client that misses the start
recovers from a snapshot, its strategy called on each of the snapshot's orders as its book is
rebuilt, deciding on a book half built, and the slicer counting them as updates. Started in turn,
none did, in three sessions under the same load. The exchange also sends a snapshot every second
rather than every minute, so a client that does recover waits a second, not up to a minute, after
which the book's client stops for want of market data, as a session's market maker once did. A session is market
data, never committed (the [charter](CHARTER.md), decision 5), so the script writes only outside the
repository or where git ignores it.

`session_system_tests` records one session and checks, besides the maker's and the taker's rules
and the truth (§3, *A replay's truth*): that each order was sent exactly as planned, its pieces'
sizes in order and no more; that each piece came its gap after the one before; that the truth marks
exactly the slicer's trades, about 150 of 550, as the recording says, worked out without the truth; and that
the recording replayed again gives the same market data, steps and truth, byte for byte. The gaps
are checked through what the market data shows: a piece reaches the matcher some updates after the
slicer decided on it, how many depending on how busy the machine is (up to 6 measured), but a late
piece doesn't delay the next, whose gap counts from the decision. So with the plan's gaps, each
piece's lateness, its updates before it arrived less the gaps' sum, is least by the same amount at
the order's end as at its start, while a slicer that miscounted would drift by an update a piece.

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

**Why the first orders take about twelve seconds.** Almost all of it is one deliberate wait: the
client sleeps 10 seconds after starting, before it trades. In a run of the debug build (its logs'
times; started from VS Code, the debugger adds a second or two of loading):

| from start | what |
|---|---|
| 0.1 to 0.5 s | the matcher builds its books: each ticker's pool of 1,048,576 orders (`ME_MAX_ORDER_IDS`), every one constructed in advance |
| 0.5 to 0.9 s | the publisher's snapshot synthesizer builds its own pool of a million updates, and clears a table of a million entries per ticker |
| 1.1 s | the order server opens its port; the "wait for the exchange" task sees it and starts the client |
| 1.3 to 2 s | the client builds its own books and pools the same way, and joins the market data |
| 2 to 12 s | the client sleeps 10 seconds before trading (`trading_main.cpp`; tests shorten it with `--warm-up-seconds`) |
| 12.4 s | its first order reaches the exchange, which publishes it, and the viewer shows it |

It once took about 28 seconds. The book's `createAndStartThread()` (`common/thread_utils.h`) slept a
second after starting each thread, and the exchange and the client start about eight each, a logger
for each part among them. The sleep was there because the new thread was given references to the
function and arguments in the caller's frame, which are gone once the call returns; the second made
it likely the thread had used them by then, and a thread not scheduled within it read freed stack.
The fork's version gives the thread its own copies, as `std::thread` does, and returns at once.

The pools are built up front so that trading never allocates memory, which is slow and takes an
unpredictable time; the book's design pays that cost once, at start. The checks inside them, and
in the queues (`ASSERT`, in `common/macros.h`), are a macro, so a check's message is built only
when it fails. The book's `ASSERT` was a function taking a `std::string`, which built the message
on every call, and that allocated: one allocation per message read from a queue, three per order
taken from a pool and returned. The debug build is
unoptimized, so building them takes most of a second. The viewer is ready from the start and waits on
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
| `src/truth/` | A replay's truth: which client sent each order, and each side of each trade (§3, *A replay's truth*) | library `fillscope_truth` |
| `src/viewer/` | The viewer: its core (no Dear ImGui) and its drawing | library `fillscope_viewer_core`, program `viewer` |
| `course/` | The course's lessons, each an answer's scenario, and its manifest (§10) | played into the build's `course/` by the target `course` |
| `src/answer/` | The answer tool: an answer's scenario played through the back end's replay, its claims checked, and the answer written for the viewer (§10) | library `fillscope_answer`, program `fillscope-answer` |
| `src/common/` | Helpers the others share: `Fd`, a file descriptor's owner; `parse_number`, strict parsing of a number from text; `write_atomically`, replacing a file so a reader never sees part of it | library `fillscope_common`, and headers |
| `tools/vscode-debug-state/` | The VS Code extension that publishes the debugger's state | JavaScript, run by VS Code |
| `tools/attribution/` | The report of who wrote each file's lines ([AI-USE.md](AI-USE.md)), Fillscope's and, with upstream's lines too, the back end's (a copy) | Python script |
| `tools/code_map/` | The check that [CODE-MAP.md](CODE-MAP.md) puts every code file in one part | Python script |
| `tests/` | One directory per area, mirroring the above | test programs per area, each in one cost tier |

Dependencies point one way: the viewer and the answer tool depend on the feed, and all three use
`src/common/`; the truth depends on nothing, and the feed never on it. Nothing in Fillscope links the back end: it is read only through its files, its market
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

**What the viewer's table is called.** One ticker's book is its **order book**: every order resting
at the exchange for that ticker, waiting to trade, bids on one side and asks on the other, each
price's orders queued in the order they will fill; traders say "the book", and "depth of book" for
the levels beyond the best. Of the three common views of a book, the viewer's is the most detailed:
level 1 is the top of the book, the best bid and ask (the two rows nearest the middle); level 2,
*market by price*, each price's total (the "level qty" column); level 3, *market by order*, every
resting order (each row). It is the book as rebuilt from the market data, not the exchange's own
copy; since this exchange publishes every add, modify and cancel, the two hold the same orders, as
`feed_system_tests` checks against the exchange's snapshots. Time-shifted, it is the book as of the
update shown (§4, *Time-shifting*). "Book" has a second meaning in trading, a trader's positions
("running a book"); here it is always the order book.

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
about 40 bytes an event, so a long run takes memory in proportion to its length. Since the books
apply updates in order, and an order's events only accumulate, its history as of an earlier update
N is the start of its history now: `history(ticker, order, N)` finds it by binary search, without a
copy. That is how a past moment has its histories (§3, *Keeping what arrives*), and why the books
the viewer shows keep none of their own (`keeping_no_histories`, §4, *Time-shifting*).

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

**A replay's truth** (`src/truth/truth.h`): the market data never says who sent an order, which is
why a trader's orders have to be inferred from it ([FEATURES.md](FEATURES.md), item 13), not read off.
A replay knows, since each request names its client, so `exchange_replay --truth FILE` writes it
down: every order the matcher accepted, by its ticker and the market order id it was given, with
its client; and every trade, by its update's number, with the client and order id of the incoming
order, which took liquidity, and of the resting one it met. The replay takes it from the matcher's
responses: an order's acceptance says its client and its id, and a trade's two fills, the incoming
order's then the resting one's, sent just before the trade's update, say who was on each side. The
format is in the back end's `exchange/matcher/replay.h`. `read_truth_file` reads it, into a `Truth`
that finds an order's client by its ticker and id, and a trade by its update. It is a library of
its own, `fillscope_truth`, which nothing in `src/feed/` links: what Fillscope works out from the
market data can't come from the truth by mistake. It is read only to show what really happened in a
replay and, once there is a detector, to score it. `session_system_tests` checks the truth of a
session of every kind of client against the recording's requests, worked out without the responses
the truth came from: the matcher numbers each ticker's orders 1, 2, 3, ... in the order it accepts
them, and it accepts every new order; a request's updates are those its step says it made; and a
trade is followed at once by its resting order's update, a cancel if the trade filled it, a modify
if not. In a session of about 30 s, that is about 770 orders and 340 trades, every kind of client on
a side of some.

**Lockstep on a replay** ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)): after each request,
`exchange_replay` calls the back end's lockstep anchor, `fillscope_request_anchor(request_number,
updates_published)`, where a debugger stops; with `--publish GROUP:PORT`, it first sends that
request's updates on a multicast group, as the exchange's publisher would. The replay is
one thread, so when the debugger stops after request N, the viewer, listening to the group, already
shows the books after it; the live exchange's publisher is a thread of its own, which a stop would
freeze mid-way. How the anchor stops a program is §6.

**Keeping what arrives** (`time_shift.h`). Live, the viewer keeps every update its books apply,
with the time it arrived, so it can show the books exactly as they were after any of them, as a
television recorder plays back what it recorded (§4, *Time-shifting*, says why). The live
books (`BookBuilder`) stay as they are; `TimeShiftBuffer` watches them. Each update is handed to it
as it arrives, before the books are given it, and after the books are given any update, incremental
or snapshot, it looks at what they applied and keeps those updates, in order. They come in **runs**:
a run starts before update 1 (from empty books), or where the books were rebuilt from a snapshot
after a gap (from a copy of the books as rebuilt), and ends where the next gap began. The books
inside a gap were never built one update at a time, so an update there can't be shown, and the
buffer says why instead; so does an update before the first kept, or after the newest.

The books after update N are a copy of the nearest **checkpoint** before it, with the updates since
applied (`apply_to`, which applies them as `BookBuilder` does: a trade becomes the ticker's last
trade, an add, modify or cancel changes its book). A checkpoint is a copy of the live books every
1,024 updates: their resting orders, a few hundred, small beside the updates. Measured over an
hour's session (180,000 updates, the live demo's rate, with about 340 resting orders; GCC 16.2.1),
the slowest moment to rebuild, 1,023 updates past a checkpoint, takes 0.4 ms in the debug build
and 1.9 ms in the sanitizer build, against a frame's 16.7 ms; without checkpoints, an hour in, it
would apply 180 times as many. The session takes about 18 MB of memory an hour, the live books'
histories of every order included. An order's history needs no checkpoint, and isn't copied: its
events as of N are the start of the live history (*Keeping the books in step*, above). So the whole of going to a
moment, the checkpoint and its updates and the books made from them (`BookBuilder::as_of`), takes
at most 0.44 ms in the debug build and 1.8 ms in the sanitizer build, an hour in, and no more
later: copying every order's history, as `as_of` once did, took 11.7 ms and 54 ms, growing with
the session.

A replay's market data is held the same way (`from_replay`): one run, from before update 1, with no
arrival times, ending where the books can't apply an update, since a replay has no snapshots to
recover with. The viewer shows a replay's moments from it, as it shows live ones (§4, *A replay, and
its addresses*), with the books after the whole replay (`Replay::books`) holding the histories.

**A live moment's address** (`kept_file.h`). Live, the viewer writes what it keeps to a file as it
keeps it, so a moment has an address: the file and an update number, from which the books then
are rebuilt exactly, however much later the question comes, while the viewer runs. The file is
`FSKEEP01`, then records, each written with one `write(2)`, so a reader of a file still being
written takes every whole record and leaves a last one cut short: a run's start (the update it
starts at, its resting orders and each ticker's last trade, since the updates before it, across a
gap, aren't in the file), and each update (its 42 bytes on the wire, and when it arrived, on the
viewer's steady clock, whose values mean something only beside each other). `read_kept_file` gives
back a `TimeShiftBuffer` (`start_run`, `append`), whose checkpoints are made as it reads.
`fillscope-books FILE --at N` prints the books after update N from such a file, as the viewer lists
them (`books_text.h`), or says why it can't.

**The viewer cleans up after itself** (`kept_files.h`): its file is `viewer-<pid>.kept` in the
`kept` directory beside the context file, deleted on every way `main` returns (closing the window,
`--exit-after`, SIGINT, SIGTERM, an error), and its last context file names none. A viewer killed
outright, or crashed, can't: a starting viewer removes the files of viewers no longer running, a
file being a running viewer's while a process with its id exists. A running viewer's is never
removed, and a dead one's whose id another process has taken waits for a later viewer.

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

**The health lines say which side has stopped.** Under the market data's state, live, come the
updates received and recoveries, the seconds since the last update, and the viewer's own paint
count ("viewer paints: 1087 (keeps rising while the exchange is stopped)"). When the books stop
changing, three things look the same on screen: the exchange is quiet or stopped (frozen at a
breakpoint, in lockstep debugging, which is why the viewer is a process of its own); the viewer's
loop has stalled (it once did, when vsync held a hidden window's swaps, and market data was lost);
or the viewer has hung. A rising paint count says the viewer is painting, so the stop is the market
data's; with the seconds since the last update, it says which side stopped. The count is also in
the line printed each second and in the log.

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
N anywhere (`ViewerApp::replay_to`), as a time-shifted moment is shown (*Time-shifting*): a few
updates forward by applying them, anything else rebuilt from the nearest checkpoint, from what is
kept of the replay (`Replay::kept`). Past an update the books can't apply, they are the last
moment before it given that update, and stay there, stuck, as a replay played through would. The
books are then exactly the recorded run's after update N, so a point
or follow carries a replay address: the market data file, the recording it came from, and N.
Applying the file's updates 1 to N rebuilds that state. Live, there is no replay address, and the
file says why and how to record a run. `--at N` opens a replay at update N, and `--ticker T` with
ticker T's book shown.

**Playing a replay, and jumping to its trades.** Below the slider, `play` moves the replay on by
itself, 10, 100 or 1,000 updates a second (a replay has no times of its own, so its pace is
chosen): each paint, `ViewerApp::advance` shows the update it has reached, the update it started from
plus the rate times the time since, so a slow paint skips updates rather than falling behind. Any
move of the user's own (a step, the slider, a dot, a jump) pauses it; it stops at the end, and play
there starts it from the start; a new rate goes on from the update shown. `< trade` and `trade >` go
to the nearest trade on the ticker shown, before or after the update shown, and in a session,
`< slicer` and `slicer >` to the nearest of the slicer's, as its truth says (`nearest_trade`, in
`session_truth.h`), so a slice can be followed piece by piece, stopping on each to look at the book.

**A session, and its truth** (`session_truth.h`). With `--session DIR`, the viewer shows a recorded
session (§1, *A recorded session*) as a replay, with its truth: who really sent each order and each
side of each trade (§3, *A replay's truth*), and from `session.json`, what each client was. It is
refused if the truth isn't its market data's: replayed from another recording, or with a trade
the market data hasn't, or without one it has. The truth is shown in two places, and nowhere else:
- **The book** names each order's client in a column, "6 (slicer)", as an answer's book names the
  scenario's traders.
- **The trades panel**, beside the book, draws each trade on the ticker shown up to the update
  shown as a dot, its update across and its price up, its area its quantity, so stepping adds the
  dots as the trades happen. The axis across spans 1,200 updates (or the whole replay, if shorter),
  labelled at its ends. It starts at update 1 and stays put while the update shown is within it, a
  line marking the update shown and the dots filling in from the left; once the update shown passes
  its end, it scrolls, the line at its right edge and its first update 1,199 before. (It first
  always ended at the update shown, so early in a replay update 1 sat partway in, its label moving
  left at each step, which the owner asked about.) The slicer's are orange,
  filled where its piece came in and took, a ring where its resting order was met; everyone else's
  grey. Hovering a dot says which clients traded; clicking it goes to its update. The panel says
  that the colours are the recording's truth, which the market data doesn't say, and what the
  slicer's order on the ticker is. In a replay without a truth, the dots are drawn with no colours
  and it says there is none; live, it says the market data never names a client.
What the panel draws is `trade_dots`, a function of the market data and the truth, which the unit
tests check; the UI tests step a session made by hand update by update, checking each dot drawn
against what its truth says.

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

### Time-shifting: pause, play, step and live
Live, the viewer can show a moment behind the newest update, as a television recorder plays back
what it recorded, from what it keeps (§3, *Keeping what arrives*). **Why** (the owner, 2026-10-26):
pointing at an order in a live book is hard. The rows move while the mouse is on its way, the order
may be cancelled or filled a moment later, and a question asked afterwards is about a book that
has moved on. A television recorder solved the same problem for a broadcast: pause, and it records
what arrives meanwhile; resume, and it plays from the recording, behind live; rewind within what it
has. It holds two `BookBuilder`s (`ViewerState`): `live`, the feed's, which takes every update
and from which the feed's health is shown and published; and `books`, the books shown, which
everything drawn and published about an order or a level is about. Following live, `ViewerApp`
gives both every update, so they are the same; time-shifted (`ViewerState::time_shift`), only
`live` takes them, and `books` are the books as of the update shown.

- **Pause** holds the books shown at the newest update; the updates arriving meanwhile are kept.
  Paused, the controls step back and forward over what is kept (across a gap, to the update on its
  other side), go to the oldest, or anywhere with the slider.
- **Play** goes on from the update shown at the pace the updates arrived: the update shown is the
  last whose arrival time is no later than that of the update play began at, plus the time since. So
  it stays as far behind live as it was when it began (the market data carries no times; these are
  the viewer's own arrival times, and the screen says so). A run's start, rebuilt from a snapshot,
  has no arrival time of its own and takes the next update's.
- **Live** follows the newest update again: the books shown become the live books, without their
  histories.

The controls sit at the bottom of the market pane, under a line saying what the screen shows, as a
video player's do: the tickers, the book and the followed order's history are a region of their
own above them (`kLiveMarket`), ending where they begin. Below the bar and the feed's lines, whose
text changes length as the market moves, they had moved a line as the mouse reached for them.

Moving the books shown forward, a few updates within a run, applies the kept updates to them, as the
live books applied them; anything else rebuilds them (`BookBuilder::as_of`): the kept books as of
the update, with the live books' recoveries up to it, which are what the books had then. A replay is
shown the same way (`show_moment`, in `viewer_app.cpp`). The books shown keep no histories, live or
replayed: an order's history is the feed's (the live books', or the replay's whole run) up to the
update shown (`shown_history`). There is one set of histories, however far back the screen is, and
a jump copies none, nor does going live. A point or a follow is about the books shown, so paused,
the order pointed at stays as the screen shows it however the live books have moved on.

While the screen isn't at the newest update, it says so, and how far behind: in the status
("showing: update 561, paused, 115 updates (1.8 s) behind live", its tooltip saying the seconds
are the viewer's arrival times; short, so it doesn't wrap as its numbers grow), in the bar ("paused at update 561, 103 updates behind (1.7 s)"), and in the context file
(`background.timeShift`: the update shown, the newest, how far behind, playing or not), where a point
or a follow says it is as of the update shown (`asOfSeq`). The feed's own lines (live, as of update
N, received, recoveries) stay the live feed's. While the viewer runs, the context file names the
file it writes what it keeps to (`background.keptFile`), and a point or a follow gives the moment's
address there (`keptAddress`: the file, and that update), from which `fillscope-books` prints the
books exactly as they were on the screen (§3, *A live moment's address*).

### The viewer's log
When the viewer fails, crashes included, the assistant has to find out why afterwards, and the
viewer's own output went with its terminal. So each run writes a log: a file in the `logs`
directory beside the context file (`.fillscope/logs/` by default), named for when the run started
and its process id (`viewer-20261026-154002-2048939.log`). It says how the viewer was started (its
command line, the build and its sanitizers, the binary and when it was built), what it showed (live
streams joined, a replay read, answers watched), the feed's events (going live, recovering and
why, each rebuild from a snapshot, an error receiving), answers shown and refused, every action of
the user's, the line it prints each second, what it said on stderr, and how it ended: the window
closed, `--exit-after`, SIGINT or SIGTERM, or an error, with the exit code. A command line it can't
read is the one thing not logged: where the log goes is part of what couldn't be read.

**Written so a crash loses nothing.** `ViewerLog::line` (`viewer_log.h`) writes each line with one
`write(2)` to a file opened for appending, with no buffer of its own, so a line is in the kernel's
hands, and survives the process, as soon as the call returns. A line the viewer also prints (each
second's report, an error) is logged first, so whoever sees it printed finds it in the log.

**A crash recorded from the crash itself.** A crash ends the process before anything orderly can
run, so `install_crash_log` (`crash_log.h`) installs handlers that record it as it happens. A fatal
signal (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT) writes a line naming it and a backtrace, straight
to the log's descriptor, using only what a signal handler may call (`write`, and `backtrace`,
loaded at install so the handler's call doesn't allocate), on a stack of its own, so a stack
overflow is recorded too; then the signal ends the process as it would have, so the exit status
and any core dump are unchanged. An uncaught exception reaches `std::terminate`, whose handler
logs its message and aborts, which the SIGABRT handler records with a backtrace. The backtrace
gives the viewer's own frames as addresses in its binary, which the log names with when it was
built: `addr2line -f -C -p -e BINARY ADDRESS...` turns them into functions, files and lines, while
the binary is unchanged. In the sanitizer build, AddressSanitizer reports SIGSEGV, SIGBUS and
SIGFPE itself, saying where and why, which a backtrace can't, so those are left to it, and the
sanitizers' reports go to a file beside the log (its name, then `.sanitizer.` and the process id).

**What it records** of the feed and answers is found by comparing the viewer's state with what the
log last said of it (`log_events`, in `log_events.h`), so each change is said once; the user's
actions are each paint's `ViewerActions`, described (`describe`).

**Bounded, and kept.** A starting viewer keeps the newest 20 logs and deletes the rest, with the
sanitizers' reports beside them, and touches nothing else in the directory; a run's log stops at 10
MB, with a line saying so (a typical hour is well under 1 MB). Logs are kept when the viewer exits,
unlike the files it makes for its own use: they are meant to be read afterwards.

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
tiers and timings are in [RUNNING.md](RUNNING.md); this chapter is how and why it works. How the
problems the tests have met were tracked down, step by step, with the experiments and commands that
narrowed each one, is in [TROUBLESHOOTING.md](TROUBLESHOOTING.md): a test that fails now and then,
CI past its time limit, windows where none should be, the machine out of memory.

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
data, recording and replay, the order server, the queue, its sockets, the client's copy of the
books; and `lowlatency_allocation_tests`, what must not allocate. The book's market maker and
liquidity taker are checked against their rules on the real exchange (`session_system_tests`, §1,
*The book's strategies*), and the same session's truth against its requests (§3, *A replay's
truth*); reading a truth file has unit tests (`truth_tests`). The test machinery is tested too
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

### Tests that write files
Here the temporary directory is in memory; in CI, it is on the runner's disk, in ext4 beneath the
container's overlay, shared with every test running at the time. There, a test that rewrites one
file over and over, truncating it each time, waits for each version to reach the disk: ext4 starts
writing a file that was truncated and rewritten as soon as it is closed (its guard for programs
that replace a file that way, `auto_da_alloc`), and truncating it again waits for that write. On an
idle disk the wait is nothing; on CI, with the back end's tests writing their logs, each took 10 to
100 ms, which took two unit tests past their 2 s, each writing a file a hundred times or more, cut
short a different way each time: `KeptFile.ReadWhileBeingWritten...` in four runs of CI, and
`TruthFile.SaysWhyItCantBeRead` in one. Measured here on an ext4 file system with other programs writing to it, the first
took 675 to 885 ms, against 278 ms on an idle one; written to a new file each time, it takes 278 ms
either way. So a test that writes many versions of a file writes each to a file of its own.

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
  so it runs again. The check's own code is `mutants.py` and the scripts it loads, less the
  mutants themselves and the names of the files they mutate: a mutant's definition holds what it
  uses of those. So adding a mutant, or a file for one, runs only that mutant, where a change to the
  check's code runs them all; the check refuses to run if its code uses a name defined among the
  mutants, which would make that untrue. The comparison is of what the build produced, not of where the change was: a
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
controls, and the frame: the introduction; a step's request, the book just before it, and its
question; or a step played. A step played keeps apart what everyone sees and what one client
sees: what it says; its market data, public, every update in words (`describe_update`), with
whose order it touches where the answer knows; what the exchange told each client, privately; the
book after it, the best bid and ask, whose order is first in line at each, and the spread
(`describe_top`); and each claim marked true or **FALSE** (with what is true instead). The words
for an update are exact, not guessed: in Ghosh's exchange a modify only ever means a resting order
partly filled, and a cancel update, in a step that sends a new order, a resting order filled
whole; in a step that cancels, it is the cancel (the back end's `MEOrderBook::match`). An order
filled whole on arrival never rests, so it has no update of its own and no row in the book: the
trade and the responses are its whole life. When no answer can be shown, the pane says why, nothing of
the last answer stays on screen, and the status line and the context bar say no answer is shown,
not that the viewer is waiting for the exchange.

**The context names where you are**: `background.answer` gives the answer file, its question, the
step (0 for the introduction), how many steps it has, and whether the step's question is shown and
the step not yet played; `background.noAnswer` says why none is shown. So a question asked in the
chat at a step is about that step, and the assistant can rebuild exactly what you see.

**Pictures of it.** `--answer-frame N` opens an answer at frame N, and `--screenshot FILE`, with
`--exit-after`, writes the window as the viewer exits (a PPM, read back from the frame just drawn).
They let the assistant see what it built, which it otherwise can't, and suit demos.

### The course
`course/` holds a course of lessons: each lesson is a scenario like any answer's, and
`course/course.json`, its manifest, lists them in order, each with a title and, once a passage of a
book is matched to it, what to read alongside (`read_course`, `course.h`). The lessons are the one
kind of explanation kept in the repository, which is only justified because nothing in them is
taken on trust: each is played through the real matcher and every claim checked, every time the
tests run.

**Built, not run by the viewer.** The build target `course` plays each lesson through the answer
tool into the build's `course/` (a lesson's answer at `course/<its name>/answer.json`), one at a
time, since each replay builds a matching engine; only lessons changed since are played again. It
is left out of the default build, because the mutation check rebuilds the back end and the tool
hundreds of times, and would replay every lesson each time. The **viewer (answers)** launch builds
it first, and `viewer --course DIR` offers it: a drop-down at the top of the answer pane lists "Your
latest question" (the answer file given with `--answer`) and each lesson. Nothing is chosen at
first, and then nothing is watched or shown, and `background.noAnswer` says so: the viewer shows
nothing the user didn't choose. Choosing one makes the viewer watch that answer, shown from its
start; a lesson's title, in the drop-down, stands in for its question. The context names the lesson
(`background.answer.lesson`), so a question asked in a lesson is about it.

**Tested as the build plays it.** `course_system_tests` plays every lesson through the tool and
`exchange_replay` into a scratch directory, reads each back as the viewer does (`load_answer`, every
claim checked again), and requires the manifest to name exactly the lessons `course/` holds. So a
lesson made false by a change to the back end or the tool fails the tests, rather than being
shown. A lesson's name has one dot, before `json`, so the build and the viewer name its directory
alike.

### The demos
Each feature is to be shown by a scripted demo that is part of the tests (charter §2, priority 3).
`viewer_demo` (`tests/viewer/viewer_demo.cpp`) holds two.

**Answers' demo** plays lesson 6, the market maker, as a person would: the viewer started as **viewer (answers)** starts it, nothing chosen; the
drop-down opened, and the lesson chosen from it; then `>` pressed to the end. After each step that
trades, it points at the resting order the trade changed (the step's explanation is about it),
then stops pointing with the context bar's `x`. At every frame it checks the screen against the
lesson's answer, read on its own: where the pane says the user is, the step's question or its
explanation, every claim present and true, and the book, one row per order resting after the step,
rebuilt from the market data rather than taken from the viewer; and at every point, that the bar
names the order pointed at. The lesson is played first, through the answer tool, so the demo
doesn't need the course built.

**The live demo** (`--live`, the test `viewer_live_demo`) shows the viewer on the real exchange,
as the **viewer + exchange + client** compound runs them, for 30 seconds from just before the
first orders arrive. It starts the exchange on ports of its own and joins the viewer to it, then
starts the client, told to warm up for a second (`--warm-up-seconds`), so trading begins about two
seconds in. At set times it shows ticker 3's book, points at the order on the top row and stops
pointing, then does the same with ticker 6's book and a row halfway down, and returns to ticker 0.
Rows away from the middle are chosen because the orders nearest the middle are the ones that
trade, and the one pointed at is likelier to stay for the seconds it is shown. The client also
cancels at random, so one may go anyway, and the bar then says so.

It checks the viewer against books of its own, built from the same market data joined on a
socket of its own. At each step, the viewer's books equal the demo's own as of the viewer's last
update, with no update missed or recovered from. The shown book's drawn rows are its first rows,
in order, none skipped: the table scrolls, so a long book's later rows aren't drawn. The rows are
read from one frame's rendered text, where asking the Test Engine about each row would take a
frame a row. While it checks or clicks a row, the demo holds the viewer's market data, so the
book doesn't change under the mouse on its way to the row; what arrives meanwhile waits in the
sockets, as it does when a paint has taken its most, and is taken in a moment later. In the
tests it waits for the orders each step needs rather than for the set times.

**The time-shifting demo** (`--time-shift`, the test `viewer_time_shift_demo`) is the live demo's
set-up showing time-shifting (§4, *Time-shifting*), over about 32 seconds. On ticker 3's book it
pauses, and the books stand still while the status line counts the updates arriving behind them;
it points at the order in view that arrived last; it steps back, an update at a time, to the one
before it arrived, where the bar says the order was never seen and its row is gone, and forward
one, where it arrives; it plays on from there, behind live; and it returns to live. Each step is
checked as the live demo checks, at the update shown, and each point's address in the kept file
rebuilds the demo's own books. To keep the steps back few, the mouse waits on `pause` and clicks
just after an order arrives in a row in view (read from the frame's text, so the click comes
within a frame or two); clicks where the mouse already rests are made at once, rather than with
the pauses of a person's hand the Test Engine's watchable speeds add to each, and the demo's own
pace keeps each step on screen. It found that the time-shifting controls could move as the mouse
reached for them, as a line above them wrapped or unwrapped with the market; they are now pinned
to the pane's bottom (§4, *Time-shifting*).

**How a demo is built.** `viewer_demo` is a program of its own, built from the demo's file and
the viewer's own drawing code (`viewer_view.cpp`, `viewer_app.cpp`: the same sources the viewer
compiles). It doesn't start the viewer program and drive it from outside. It holds a `ViewerApp`
itself, in a window the Test Engine opens, and each frame does what the viewer's loop in
`src/viewer/main.cpp` does: take in market data, look for an answer, paint. So what a demo shows is
drawn by the viewer's own code, but the window, the loop, and its pacing are the demo's. The
viewer's `main` (its GLFW setup, its 60 paints a second, its signals, `--exit-after`) is tested
apart, as a user runs it, by `viewer_program_tests`.

A demo is a Test Engine test with two halves. The *GUI function* is one frame of the viewer,
called every frame. The *test function* is the script: it runs as a coroutine beside the frames,
and acts as a person would, through the Test Engine's own mouse: it moves it to an item, found by
its Dear ImGui identifier (`**/3##ticker`, ticker 3's button), clicks, scrolls with the wheel, and
waits. Its checks read what the viewer did, never a picture of it: the viewer's state (its books,
what is pointed at), where the Test Engine saw each item drawn, and the frame's rendered text,
which Dear ImGui's logging (`LogToBuffer`) captures as text while the frame is drawn. Recording is
a flag, `--record FILE.mp4`: the same run, with the Test Engine's capture tool reading each frame
back from the window's OpenGL framebuffer and piping it to `ffmpeg` (below).

Each is one script run three ways, through the Dear ImGui Test Engine, which drives the UI tests too.
The waits are the Test Engine's own (`Sleep`), skipped at full speed, so the checks are the same
every way, and a demo that shows something false fails like any test.
- *In the tests,* on the Test Engine's null backend at full speed: answers' about 1.5 seconds,
  the live ones about 10 (the exchange's start, the client's, and the orders the steps wait for),
  with no window, at the viewer's own size and look (`window_size`, `apply_viewer_look`).
- *Watched* (`--watch`, the **demo: a market maker**, **demo: live trading** and **demo: time-shifting**
  launches), in a real window through GLFW and
  OpenGL, the viewer's size, at the Test Engine's *cinematic* speed, the mouse slow enough to
  follow; four seconds a frame.
- *Recorded* (`--record FILE.mp4`, the **demo: record a market maker**, **demo: record live
  trading** and **demo: record time-shifting** launches), for sharing (on
  LinkedIn, say): the Test Engine reads each frame back from the window and pipes it to `ffmpeg`,
  so the video holds the viewer's window alone, pixel for pixel, its drawn mouse included, at 30
  frames a second. It is made for a viewer on a laptop, in a hurry, who pauses to read a frame in
  full: a 16:9 window of 1280 by 720, smaller than the viewer's, so the text is larger in the
  video; the Test Engine's normal speed, the mouse quick; two seconds a frame. Answers' is about 80 seconds
  and 5 MB; the live one, 30 seconds and 2 MB; the time-shifting one, 32 seconds. No screen recorder is involved, and a new recording after a change is one command,
  with the same moves and timing every time.

**Moving as a person would.** What the video shows has to be caused by the mouse, or it looks
like a glitch. The mouse rests where no tooltip covers the frame, near the next control it clicks
(beside the answer's controls; below the book while pointing). A frame longer than the pane is
scrolled to its end with the mouse's wheel, a notch at a time, and back to the top before `>` is
pressed: left to itself, the Test Engine would bring an item scrolled out of view back into view
with no mouse in sight. The drop-down stays open long enough to read a few lessons.

**What recording needs.** The capture must keep other windows visible
(`ImGuiCaptureFlags_IncludeOtherWindows`): by default it hides them, and the drop-down's list is a
window of its own. It reads the framebuffer in the window's coordinates, so on a display at scale
2 it would record a quarter of the window, magnified; the recording's window asks GLFW for a
framebuffer of its own size (`GLFW_SCALE_FRAMEBUFFER` off), so the video is the same on any
display. The video is H.264, which players and sites accept widely: `libx264` where `ffmpeg` has
it, otherwise `libopenh264`, and no recording at all where it has neither, since left to choose,
`ffmpeg` falls back to MPEG-4 Part 2 (Fedora's `ffmpeg-free` has no `libx264`), which many players
can't play. The Test Engine sends at most 30 frames a second but fewer when drawing and reading
back a frame takes longer, as in the debug build, and doesn't repeat one to make up; told the rate
was 30, `ffmpeg` made a video that played faster than the demo ran (75 seconds for 80). So each
frame is stamped with the time it arrives (`-use_wallclock_as_timestamps`), and the video is made
at 30 frames a second from them, a frame repeated where none came: it plays in the time the demo
took.

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
