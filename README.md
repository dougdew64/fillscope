# Fillscope

**Purpose:** what Fillscope is, what it will do, how it's built, and where everything else is.
**Status:** reference; the front page. **Read when:** first, before any other document.

**An explainable order-book backtester: click any order, ask why it filled.**

> **Status:** just started (October 2026). The back end is a fork of the trading system built in
> Sourav Ghosh's *Building Low Latency Applications with C++*, built and tested by Fillscope. The
> viewer shows its exchange's order books, rebuilt from the exchange's market data, live or from a
> recorded run replayed exactly to any point, and publishes what you point at and follow for an
> assistant; the debugger's state is published too. A debugger steps the matching engine through a
> recorded run, one request at a time, while the viewer follows ([docs/PLAN.md](docs/PLAN.md)); the
> rest of this README describes the plan.

A backtest replays history and simulates how a strategy's orders would have been filled. Its results
rest on assumptions that are usually invisible: where an order sat in the queue at its price, how
much latency it had, what traded and cancelled ahead of it. Fillscope makes those assumptions
inspectable.

## What it will do
Fill explanations grounded in exact state, per-order counterfactuals made exact by deterministic
replay, how much of the profit is an assumption, and an animated replay of the book. Each is
described in the feature backlog, [docs/FEATURES.md](docs/FEATURES.md). The matching engine
stepped in a debugger while the book view updates with each line is already built.

## How it's built
- **The code that shows modern C++ design is hand-written,** readable and tested. Which code that
  is gets decided stage by stage.
- **The rest is built with AI assistance (Claude):** so far the market data reader and the Dear
  ImGui viewer of the books, the debug-state extension, the attribution report, the code map's
  check, the test infrastructure and tests of all the code, the hand-written code included, and
  the changes to the back end (deterministic replay, the lockstep anchor, its bug fixes and tests).
  [AI-USE.md](docs/AI-USE.md) lists each part and what checks it.
- **No AI runs inside the app.** The app makes its state exactly observable and writes the context
  of whatever you point at to a file, `.fillscope/context.json`, replaced on every change; an
  assistant working beside it in the editor reads that context and the source, and will run
  counterfactual replays from the command line.
- **Every feature is demonstrable:** scripted UI tests drive the viewer with real clicks; they are
  planned to record the demo videos too ([docs/FEATURES.md](docs/FEATURES.md)).

## Roadmap
The back end, an exchange with its matching engine and trading clients, is a fork of the system
in Ghosh's book ([PacktPublishing/Building-Low-Latency-Applications-with-CPP](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP),
MIT). Fillscope is its front end, and the fork is changed where Fillscope needs it.
1. The fork, built by Fillscope at a pinned commit, and tested where Fillscope relies on it.
2. A live view of the exchange's book, rebuilt from its market data.
3. Deterministic replay: the exchange's sequenced input recorded and replayed, giving the same
   output.
4. The matching engine stepped in a debugger while the book view follows.

These four are built. The rest, from ITCH 5.0 replay to measured throughput, is in the feature
backlog, [docs/FEATURES.md](docs/FEATURES.md).

## Data
Market data is never committed. Tests use small hand-made message files; real days come from
Nasdaq's public ITCH samples, downloaded locally.

## Documents
[docs/CHARTER.md](docs/CHARTER.md) (purpose, priorities, decisions) ·
[docs/DECISIONS.md](docs/DECISIONS.md) (the record) · [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (how the code works) ·
[docs/PLAN.md](docs/PLAN.md) (next steps) · [docs/FEATURES.md](docs/FEATURES.md) (features to build) ·
[docs/TECH-DEBT.md](docs/TECH-DEBT.md) (known debt) · [docs/RUNNING.md](docs/RUNNING.md) (build, run, test) ·
[docs/WORKING-WITH-GIT.md](docs/WORKING-WITH-GIT.md) (git and the back end, step by step) ·
[docs/AI-USE.md](docs/AI-USE.md) (how AI is used here, and what checks it) ·
[docs/ATTRIBUTION.md](docs/ATTRIBUTION.md) (who wrote each file's lines) ·
[docs/DEBUGGER-LOCKSTEP.md](docs/DEBUGGER-LOCKSTEP.md) (stepping the engine while the animation
follows) · [CLAUDE.md](CLAUDE.md) (how the AI assistant
works in this repository).

## License
MIT: see [LICENSE](LICENSE).
