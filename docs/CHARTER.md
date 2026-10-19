# Fillscope charter

**Purpose:** what Fillscope is for, what ranks above what, and its standing decisions.
**Status:** authority. Amended deliberately by the project's owner; history goes in
[DECISIONS.md](DECISIONS.md). **Read when:** any design or scope question.

## 1. Purpose
Fillscope is a **learning instrument first**: an order-book simulator and backtester built so that
how order books, matching, fills and backtest validation work can be understood from the inside,
and so that every result it shows can be explained from its exact state.

It is also a demonstration of **judgement in using AI to write code**: where AI is used, where it
isn't, and how its output is checked (decision 7).

## 2. Priorities, in order
1. **Correctness and explainability.** A claim, an explanation or a display that isn't exactly true
   is a defect, however good it looks. Every explanation is grounded in the simulator's exact state.
2. **Hand-written C++ where it shows modern C++ design.** The owner writes the code that
   demonstrates modern C++ design, readable and tested; which code that is gets decided stage by
   stage (§4). Claude writes the rest, including the UI and tooling.
3. **Every feature demonstrable.** Each feature is shown by a scripted demo, which is part of the
   test suite.
4. **Polish: as much as compelling demo videos need, and no more.**

Where two priorities conflict, the higher wins.

## 3. Decisions
1. **What it is:** an observatory for its own order-book simulator and backtester.
2. **The features that distinguish it:** fill explanations grounded in exact state; per-order
   counterfactuals made exact by deterministic replay; how much of a result depends on the queue and
   latency assumptions; an animated replay; debugger lockstep with the matching engine.
3. **No AI in the app.** The app makes its state observable and logs the context of what is
   selected; an assistant works beside it in the editor, reading the source and that context, and
   runs counterfactual replays through a command-line tool.
4. **C++ on Linux throughout,** including the UI: Dear ImGui with ImPlot. Everything builds with
   GCC, with AddressSanitizer and UndefinedBehaviorSanitizer, and ThreadSanitizer for threaded code.
5. **Tests below the UI first:** core unit tests, replay fidelity checks, the command-line tool. UI
   tests and demo recordings with the Dear ImGui Test Engine. **Claude tests all the code
   aggressively, the owner's included,** so that nothing is built on undetected defects; the owner
   writes some tests too.
6. **Market data is never committed.** Hand-made message files for unit tests, a small synthetic
   generator for demos, real days downloaded locally.
7. **AI output is trusted only through a check that doesn't depend on the AI.** Each use of AI
   answers three questions: is understanding the point (then the code is hand-written)? Can the
   output be checked independently of the AI? Is it reproducible (prefer AI-built artifacts that a
   deterministic step turns into code)? Each use, its check, and the defects the checks caught are
   documented publicly. The proposed ITCH parser (§4) is the first worked example: AI builds a
   model of the specification, checked mechanically and against an independent model; a
   deterministic generator writes the decoders; they must agree with the hand-written ones on every
   message of a real trading day.
8. **Several processes and threads, not one.** The parts of the system communicate over sockets,
   as real trading systems do. The exact split, and how replay stays deterministic across it, are
   decided as the design develops ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)).

## 4. Open questions
- Which code the owner writes: decided stage by stage, as the owner learns each stage's domain.
- The ITCH parser: hand-written for the messages the book needs, the rest generated from a model of
  the specification checked against it (proposed).
