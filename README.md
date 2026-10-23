# Fillscope

**An explainable order-book backtester: click any order, ask why it filled.**

> **Status:** just started (October 2026). The plumbing is built: a stand-in source of frames, a
> viewer, the socket between them, stepping both in a debugger in lockstep, and publishing what you
> point at for an assistant. The order book is not built yet; the rest of this README describes the
> plan.

A backtest replays history and simulates how a strategy's orders would have been filled. Its results
rest on assumptions that are usually invisible: where an order sat in the queue at its price, how
much latency it had, what traded and cancelled ahead of it. Fillscope makes those assumptions
inspectable.

## What it will do
- **Fill explanations grounded in exact state.** Click an order and ask "why didn't this fill?". The
  answer comes from the simulator's exact state at that moment: the book on arrival, the queue
  ahead, the trades and cancels that followed, the latency applied.
- **Per-order counterfactuals, made exact by deterministic replay.** "What if this order had arrived
  50 µs earlier?" is answered by replaying from a snapshot with one thing changed, not by estimating.
- **How much of the profit is an assumption.** The same strategy with and without its queue and
  latency models, and how far the results move.
- **An animated replay of the book,** and the matching engine stepped in a debugger while the book
  view updates with each line.

## How it's built
- **The code that shows modern C++ design is hand-written,** readable and tested. Which code that
  is gets decided stage by stage.
- **The rest is built with AI assistance (Claude):** so far the Dear ImGui viewer, the test
  infrastructure, and tests of all the code, the hand-written code included; later, the replay
  command-line tool.
- **No AI runs inside the app.** The app makes its state exactly observable and writes the context
  of whatever you point at to a file, `.fillscope/context.json`, replaced on every change; an
  assistant working beside it in the editor reads that context and the source, and will run
  counterfactual replays from the command line.
- **Every feature is demonstrable:** scripted UI tests drive the viewer with real clicks; they are
  planned to record the demo videos too.

## Roadmap
1. A book with resting limit orders on both sides; top of book and depth.
2. Matching: price-time priority, partial fills, trades.
3. Cancel and modify by order id; IOC and fill-or-kill.
4. Replay of Nasdaq TotalView-ITCH 5.0 data into a book builder.
5. Backtesting on simulated time, with deterministic replay and recovery from a snapshot.
6. Fill simulation: queue position, latency, fees.
7. Validation: how far the results can be trusted, and a written report.
8. Throughput: measured messages per second.

## Data
Market data is never committed. Tests use small hand-made message files; real days come from
Nasdaq's public ITCH samples, downloaded locally.

## Documents
[docs/CHARTER.md](docs/CHARTER.md) (purpose, priorities, decisions) ·
[docs/DECISIONS.md](docs/DECISIONS.md) (the record) · [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (how the code works) ·
[docs/PLAN.md](docs/PLAN.md) (next steps) · [docs/TECH-DEBT.md](docs/TECH-DEBT.md) (known debt) · [docs/RUNNING.md](docs/RUNNING.md) (build, run, test) ·
[docs/AI-USE.md](docs/AI-USE.md) (how AI is used here, and what checks it) ·
[docs/ATTRIBUTION.md](docs/ATTRIBUTION.md) (who wrote each file's lines) ·
[docs/DEBUGGER-LOCKSTEP.md](docs/DEBUGGER-LOCKSTEP.md) (stepping the engine while the animation
follows) · [CLAUDE.md](CLAUDE.md) (how the AI assistant
works in this repository).

## License
MIT: see [LICENSE](LICENSE).
