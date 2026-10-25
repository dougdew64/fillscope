# Feature backlog

**Purpose:** the features envisioned for Fillscope and not yet built, each with what it is and what
it rests on. **Status:** live plan; an item is removed when it is built (git keeps it), and the file
is deleted when empty. **Read when:** choosing what to build next, or asking what Fillscope will do.

The items aren't in order yet: they are ordered, and the next ones broken into steps in
[PLAN.md](PLAN.md), after the pause that follows its steps 1 to 10. The [charter](CHARTER.md)'s
rules bind every one of them. Known defects and test gaps are in [TECH-DEBT.md](TECH-DEBT.md).

Built so far: the back end, built and tested by Fillscope at a pinned commit; the live view of the
exchange's books; deterministic replay; and the matching engine stepped in a debugger while the
book view follows ([ARCHITECTURE.md](ARCHITECTURE.md)).

## What Fillscope shows
The features that distinguish it.
1. **Fill explanations grounded in exact state.** Click an order and ask "why didn't this fill?".
   The answer comes from the simulator's exact state at that moment: the book on arrival, the queue
   ahead, the trades and cancels that followed, the latency applied. Rests on fill simulation (7).
2. **Per-order counterfactuals, made exact by deterministic replay.** "What if this order had
   arrived 50 µs earlier?" is answered by replaying from a snapshot with one thing changed, not by
   estimating. Rests on deterministic replay (built) and recovery from a snapshot (6).
3. **How much of the profit is an assumption.** The same strategy with and without its queue and
   latency models, and how far the results move. Rests on fill simulation (7).
4. **An animated replay of the book.**

## What the back end gains
The stages of the roadmap, each a change to the fork.

5. **Replay of Nasdaq TotalView-ITCH 5.0 data into the exchange.** Its parser is to be the first
   worked example of checking AI output (charter §3, decision 6). Proposed, not yet decided:
   hand-written decoders for the messages the book needs, and the rest generated. AI builds a
   model of the specification, checked mechanically and against an independent model; a
   deterministic generator writes the decoders from it; they must agree with the hand-written ones
   on every message of a real trading day. The data is downloaded locally, never committed.
6. **Backtesting on simulated time, with recovery from a snapshot.** Latency in a backtest is
   simulated, not the network's ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)).
7. **Fill simulation:** queue position, latency, fees.
8. **Validation:** how far the results can be trusted, and a written report.
9. **Throughput:** measured messages per second.

## Tools around the app
10. **A command-line tool for counterfactual replays,** which an assistant working beside the app
    runs, since no AI runs in it (charter §3, decision 2). It is tested below the UI, with the
    core unit tests and replay fidelity checks.
11. **A small synthetic market data generator for demos,** since market data is never committed
    (charter §3, decision 5).
12. **Demo recordings:** each feature shown by a scripted demo that is part of the test suite
    (charter §2, priority 3), recorded with the Dear ImGui Test Engine, which already drives the
    viewer's UI tests (charter §3, decision 4).
