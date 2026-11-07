# The back end, one order's life

**Purpose:** how the back end's code works, explained by following one order from the client that
decides to send it, through the exchange, and back to every client as market data. **Status:**
reference for the steps written; the route below says which are still to come. **Read when:**
reading the book's code, or before changing the back end.

The back end (`backend/`) is the trading system from Ghosh's book, forked. The book builds it part
by part, a chapter at a time: the building blocks, then the exchange, then the client. This
document crosses those parts the other way, along the path one order takes through the running
system, so each part is seen doing its job for the next. It explains the code in the fork, which
is the book's final version (`backend/Chapter12/`) with Fillscope's changes. Where the fork
changes what the book printed, it says so. `git diff 11e3925 fillscope -- FILE`, run in
`backend/`, shows exactly what changed in a file; `11e3925` is the book's repository as the fork
found it. The fork's README lists every change, the bugs it fixed and its known issues.

What the strategies decide, rule by rule, is in [ARCHITECTURE.md](ARCHITECTURE.md) §1, *The
book's strategies*, and *The planted slicer*. This document is about the machinery underneath.
Paths are under `backend/Chapter12/` unless they say otherwise.

## The whole trip on one page
**The programs.** One exchange (`exchange_main`) and several trading clients (`trading_main`), each
client a process of its own. Each client holds a TCP connection to the exchange, on which it sends
its requests (a new order, a cancel) and receives the exchange's private answers to them
(accepted, filled, canceled). The exchange publishes what happens to its books as market data,
over multicast: one copy sent once, received by every client and by Fillscope's viewer. The
market data names no client.

```
                          requests (TCP)
   trading_main  ------------------------------------->  exchange_main
   (one per client) <-------------------------------------  the matching engine
                          responses (TCP)                 and its books
          ^                                                    |
          |              market data (multicast)               |
          +----------------------------------------------------+
                          to every client, and the viewer
```

**Inside each program,** every component is a thread, and the threads hand each other work through
lock-free queues, each with one writer and one reader:

```
exchange_main:  order server --requests--> matching engine --market updates--> market data publisher --> snapshot synthesizer
                     ^                          |                                   |                         |
                     +--------responses---------+                         incremental stream          snapshot stream

trading_main:   market data consumer --market updates--> trade engine --requests--> order gateway
                                                              ^                          |
                                                              +--------responses---------+
```

**One order's trip.** A client's trade engine sees a market update and its strategy decides to
buy (step 1). The order gateway sends the request over TCP (step 2). The exchange's order server
receives it, and with whatever else arrived at the same time, passes them to the matching engine
in the order they arrived (step 3). The matching engine matches it against that ticker's book,
or rests it there (step 4). Each client involved gets a response over TCP; everyone gets the
market data, both as each change (the incremental stream) and, now and then, as the whole book
(the snapshot stream) (step 5). Every client applies the changes to its own copy of the book,
and uses a snapshot to recover if it misses one. The changed book is what its strategy decides
on next, so the trip begins again (step 6).

## The route
| Step | What happens | The code | Where the book first builds it | Written |
|---|---|---|---|---|
| 1 | A client decides to trade | `trading/strategy/` | `Chapter8/` | yes, below |
| 2 | The order leaves the client, over TCP | `trading/order_gw/` | `Chapter8/` | not yet |
| 3 | The exchange receives it, and puts every client's requests in one order | `exchange/order_server/` | `Chapter7/` | not yet |
| 4 | It is matched against the book | `exchange/matcher/` | `Chapter6/` | not yet |
| 5 | Responses go to the clients over TCP, market data to everyone over multicast | `exchange/order_server/`, `exchange/market_data/` | `Chapter7/` | not yet |
| 6 | Each client rebuilds the book, recovering from gaps, and step 1 begins again | `trading/market_data/`, `trading/strategy/market_order_book.*` | `Chapter8/` | not yet |

Underneath every step are the building blocks in `common/`: the lock-free queue, the memory pool,
the logger, the sockets and the threads (`Chapter4/` in the fork). Each is explained where the
route first needs it.

## 1. A client decides to trade
**In short:** everything a client decides happens on one thread, the trade engine's. A market
update changes the client's copy of the book, and the engine passes the change to its strategy.
The market maker and the liquidity taker tell an order manager what prices they want; it keeps one
order a side, and asks a risk check before each new one. The slicer and the random client send
their orders directly. Every request ends in the queue to the order gateway.

### A client's threads, and the queues between them
A trading client, `trading_main`, is one process with eight threads. Three are the components it
starts: the **market data consumer**, which receives the exchange's market data (step 6); the
**order gateway**, which holds the TCP connection to the exchange (step 2); and the **trade
engine**, which decides. The fourth is `main`'s own, which starts the others, waits ten seconds
(the warm-up; Fillscope's sessions and tests shorten it with `--warm-up-seconds`), and then, for
every strategy but the random client, only waits for the run to end. Each of the four has a
logger, and each logger a thread of its own, which takes what its owner logged from a lock-free
queue and writes it to the file (`common/logging.h`), so logging costs the trading thread only a
copy into the queue.

The threads share nothing but three lock-free queues, which `main` makes
(`trading/trading_main.cpp:88`):

```
market data consumer --market updates-->  trade engine  --requests--> order gateway --TCP--> exchange
                                               ^                            |
                                               +--------responses-----------+
```

Each queue has exactly one writer and one reader, which is all the book's queue allows
(`common/lf_queue.h`; [ARCHITECTURE.md](ARCHITECTURE.md) §1, *How the exchange's threads hand each
other work*). Everything a strategy decides happens on the trade engine's thread, so its order
book, its features, its orders and its position need no locks: no other thread touches them.

The random client breaks the pattern. Its orders are made in `main` (`trading_main.cpp:139`),
not in a strategy, and `main` writes them to the request queue. The trade engine still runs, but
with no strategy it only logs what it is told and never writes to that queue, so the queue still
has one writer. Its logger's queue doesn't: `main` sends each order through the engine's
`sendClientRequest()`, which logs it in the engine's logger, from `main`'s thread, while the
engine's thread logs there too. The two writers garble each other: in the 2026-10-27 session, 827
of the random client's 14,688 log lines were malformed, and none of the other clients'. It is a
defect in the book's code, to be fixed in the fork next.

### The trade engine's loop
`TradeEngine::run()` (`trading/strategy/trade_engine.cpp:79`) never sleeps. Round and round, it
first takes every response waiting from the order gateway, then every market update waiting from
the consumer, handling each before taking the next. Spinning costs a whole core, and buys a
reaction that doesn't wait for the operating system to wake the thread. Responses come first, so
the strategy knows the state of its own orders before it reacts to the market.

`run_`, the flag that ends the loop, is a `volatile bool` that `main`'s thread writes and the
engine's thread reads. In C++ that is a data race, undefined behaviour: `volatile` makes the
compiler read it each time, but promises nothing between threads. `std::atomic<bool>` is the type
that does. It works in practice on x86, and the book's other components do the same.

### From a market update to a decision
A market update goes to the client's copy of that ticker's book,
`MarketOrderBook::onMarketUpdate()` (`trading/strategy/market_order_book.cpp:20`). An add, a
modify or a cancel changes the book; then the book calls the engine's `onOrderBookUpdate()`. A
trade changes nothing, since the exchange sends the change to the order it filled as an update of
its own; the book calls the engine's `onTradeUpdate()` at once. A response from the exchange goes
to the engine's `onOrderUpdate()`. Each of the three does the engine's own bookkeeping first, then
calls the strategy:

| Engine's call | First | Then the strategy's |
|---|---|---|
| `onOrderBookUpdate` | the position keeper's unrealized profit, at the new best prices; the feature engine's fair price | `algoOnOrderBookUpdate_` |
| `onTradeUpdate` | the feature engine's trade ratio | `algoOnTradeUpdate_` |
| `onOrderUpdate` | on a fill, the position keeper's position and profit | `algoOnOrderUpdate_` |

The three `algoOn…` members are `std::function`s (`trade_engine.h:76`). The engine's constructor
sets them to functions that only log, then makes the strategy it was asked for, whose own
constructor replaces them with its own (`trading/strategy/market_maker.cpp:10`). So the engine
calls a strategy without knowing which it has. The price is a call through a type-erased wrapper
that the compiler can't inline, on every update; a template parameter, or a `switch` on the
strategy's type, would let it inline.

Each strategy uses only some of the three:

| Strategy | Order book update | Trade | Response |
|---|---|---|---|
| Market maker | requotes both sides | ignores | to its order manager |
| Liquidity taker | ignores | may follow the trade | to its order manager |
| Slicer | counts it, may send a piece | counts it, may send a piece | cancels a piece once accepted |
| Random client | (logs only; its orders come from `main`) | | |

### The features
The **feature engine** (`trading/strategy/feature_engine.h`) computes the two numbers the
strategies decide on. The **fair price**, on each book update, is the best bid and ask each weighted
by the quantity on the other side: `(bid × ask_qty + ask × bid_qty) / (bid_qty + ask_qty)`. With
more waiting to buy than to sell it sits nearer the ask, where the price is more likely to go. The
**trade ratio**, on each trade, is the trade's quantity over the quantity at the best price on the
side it took from. The trade's side is the side of the incoming order
(`exchange/matcher/me_order_book.cpp:42`), so a buy took from the best ask. Since the trade comes
before the update that shrinks or removes the order it filled, that quantity still includes the
filled order. A ratio of 1 means the trade took the whole best level.

The engine keeps one of each number for the whole client, not one per ticker. That is correct only
because each number is read in the same call that wrote it, for the same ticker, and the strategy
checks the same condition the engine did before writing (both sides of the book quoted). If either
check changed, a strategy could act on another ticker's number.

### The order manager: one order a side
The market maker and the liquidity taker don't send orders themselves. They tell the **order
manager** (`trading/strategy/order_manager.h`) the prices they want, a bid and an ask on a ticker,
or `Price_INVALID` for none: `moveOrders(ticker, bid, ask, clip)`. The manager keeps at most one
order on each side of each ticker, an `OMOrder` (`om_order.h`), and moves it toward the price
wanted, one step at a time, through its states:

```
INVALID or DEAD --new order sent--> PENDING_NEW --accepted--> LIVE --price wanted changes: cancel sent--> PENDING_CANCEL --canceled--> DEAD
                                                                |
                                                                +--filled to nothing--> DEAD
```

A change of price is a cancel, then, on a later update once the cancel is confirmed and the order
is `DEAD`, a new order. Nothing is sent while an order is pending, so a strategy can't send a
second order on a side before the exchange has answered the first. The fork adds one transition:
an order the exchange `REJECTED` (step 3) is `DEAD`, so the strategy can send another.

Only the price is compared. The comment on `moveOrders` (`order_manager.h:102`) says an order not
of the wanted quantity is cancelled too, but `moveOrder` checks only `order->price_ != price`. So a
partly filled order rests with what is left until the price wanted moves. In the session recorded
on 2026-10-27, the market maker's sell of 200 on ticker 3 at 181 (its order 18) was filled 112,
and rested with 88 for another 0.27 seconds, until the maker wanted a different ask and cancelled
it (its log, `trading_engine_1.log`).

### The risk check
Before every new order, the manager asks the **risk manager** (`trading/strategy/risk_manager.h`).
It refuses an order larger than the ticker's largest order, one that would take the position past
the largest position, and any order once the ticker's total profit is below its largest loss.
These are the last three of each ticker's five settings on `trading_main`'s command line. A refused
order is only logged; the manager tries again on the next update. The check counts the position
already filled, not orders still resting, so a resting order's fill can take the position past
its limit.

The **position keeper** (`trading/strategy/position_keeper.h`) keeps each ticker's position and
its profit, realized and unrealized, from the fills and the best prices. The fork fixed it on
2026-11-07: the sums each side's average price comes from were never initialized, so a fill was
added to whatever the memory held, and so was the profit the risk check reads (the fork's README).

The settings Fillscope's sessions use are the book's own (`scripts/run_clients.sh`, where the
liquidity taker is commented out), and some of them refuse orders. In the 2026-10-27 session,
recorded before that fix:
- **The liquidity taker never trades ticker 2.** Its clip there, 350, is larger than its largest
  order there, 250, so every order it wanted was refused, 43 times (`ORDER_TOO_LARGE`).
- **The liquidity taker's clip on tickers 0 and 7, 300, is its largest position there,** so once
  an order filled in full it could only trade back the other way.
- **The market maker was refused most on tickers 2 and 7:** 125 and 113 times for its loss, 49 and
  126 for its position.

### The slicer and the random client go around it
The slicer sends its pieces itself, through `TradeEngine::sendClientRequest()`, numbering them 1,
2, 3 and so on on each ticker, and checks no risk: it is planted to send exactly its orders
([ARCHITECTURE.md](ARCHITECTURE.md) §1, *The planted slicer*). The random client's orders, from
`main`, go around the manager and the risk check too.

Whichever way it is made, a request ends in `sendClientRequest()` (`trade_engine.cpp:69`), which
copies it into the next free slot of the request queue and publishes the slot. That ends step 1.
The order gateway's thread takes it from there (step 2).
