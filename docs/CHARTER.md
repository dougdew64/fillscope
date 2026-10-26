# Fillscope charter

**Purpose:** what Fillscope is for, what ranks above what, and its standing decisions.
**Status:** authority. Amended deliberately by the project's owner; history goes in
[DECISIONS.md](DECISIONS.md). **Read when:** any design or scope question.

## 1. Purpose
Fillscope is a **learning instrument first**: an observatory for a trading system, so that how
order books, matching, fills and backtest validation work can be understood from the inside, and
so that every result it shows can be explained from its exact state. The trading system is a fork
of the one built in Sourav Ghosh's *Building Low Latency Applications with C++* (decision 1), so
the domain is learned from the book, its code, and changes to that code.

It is also a way to **learn modern C++ from the inside**: its code, the AI-written parts included,
is written to be learned from, and an assistant reading the debugger's exact state explains what
each step does.

It is also a demonstration of **judgement in using AI to write code**: where AI is used, where it
isn't, and how its output is checked (decision 7).

## 2. Priorities, in order
1. **Correctness and explainability.** A claim, an explanation or a display that isn't exactly true
   is a defect, however good it looks. Every explanation is grounded in the simulator's exact state.
2. **Hand-written C++ where it shows modern C++ design.** The owner writes the code that
   demonstrates modern C++ design, readable and tested, in Fillscope or in the fork; which code
   that is gets decided stage by stage (§4). Claude writes the rest, including the UI and tooling.
3. **Every feature demonstrable.** Each feature is shown by a scripted demo, which is part of the
   test suite.
4. **Polish: as much as compelling demo videos need, and no more.**

Where two priorities conflict, the higher wins.

## 3. Decisions
1. **What it is:** the front end, an observatory, for a fork of the trading system in Ghosh's book
   ([PacktPublishing/Building-Low-Latency-Applications-with-CPP](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP),
   MIT): its exchange and trading clients are the back end. The fork is a public repository of its
   own, changed on a branch; Fillscope pins one commit of it. What is built is in
   [ARCHITECTURE.md](ARCHITECTURE.md), and what is to be built in [FEATURES.md](FEATURES.md).
2. **The assistant isn't in the app.** The app makes its state observable and publishes the context
   of what is selected, to a file it replaces on every change (`.fillscope/context.json`); the
   assistant works beside it in the editor, reading the source and that context. The rule is about
   the assistant, not about methods: the app may use whatever its features need, a model trained
   offline among them (a pattern detector, say).
3. **C++ on Linux throughout,** including the UI: Dear ImGui with ImPlot. Everything builds with
   GCC, with AddressSanitizer and UndefinedBehaviorSanitizer, and ThreadSanitizer for threaded code.
4. **Tests below the UI first:** core unit tests and replay fidelity checks; UI tests with the Dear
   ImGui Test Engine. **Claude tests all the code aggressively, the owner's included,** so that
   nothing is built on undetected defects; the owner writes some tests too. In the fork, what Fillscope relies on and every change made to it are
   tested; the upstream code as a whole is not retrofitted with tests. A bug found in it is fixed
   in the fork, with a test, and recorded.
5. **Market data is never committed.** Hand-made message files for unit tests; real days
   downloaded locally.
6. **AI output is trusted only through a check that doesn't depend on the AI.** Each use of AI
   answers three questions: is understanding the point (then the code is hand-written)? Can the
   output be checked independently of the AI? Is it reproducible (prefer AI-built artifacts that a
   deterministic step turns into code)? Each use, its check, and the defects the checks caught are
   documented publicly.
7. **Several processes and threads, not one.** The parts of the system communicate over sockets,
   as real trading systems do. The exact split, and how replay stays deterministic across it, are
   decided as the design develops ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)).

## 4. Open questions
- Which code the owner writes: decided stage by stage, as the owner learns each stage's domain.
