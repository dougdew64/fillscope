# Decisions: the record

**Purpose:** what was decided, when and why, including what was later reversed.
**Status:** record; binds nothing (the current answer is in the [charter](CHARTER.md) or the code).
**Read when:** wondering why something is the way it is.

Newest first.

| Date | Decision |
|---|---|
| 2026-10-23 | Fillscope becomes the front end for a fork of the trading system in Sourav Ghosh's *Building Low Latency Applications with C++* (was: an observatory for its own simulator and backtester), so the domain is learned from the book, its code and changes to it. The fork is a public repository of its own, worked on a branch, and Fillscope pins one commit of it; deterministic replay is its first substantial change; its attribution report separates upstream lines; what Fillscope relies on and every change are tested, not the upstream code as a whole; bugs found upstream are fixed in the fork. Claude implements the change ([PLAN.md](PLAN.md)); the owner's own changes to the fork are the owner's. Roadmap stages 1 to 3 (book, matching, cancel and modify) come from the fork. |
| 2026-10-23 | Attribution is mechanical: a commit is the owner's or co-authored by Claude, never mixed, and which code is the owner's is decided stage by stage, never in advance. A generated report, [ATTRIBUTION.md](ATTRIBUTION.md), gives each file's lines from each kind of commit (`git blame`), as of a named commit, so the claim can be checked. |
| 2026-10-20 | ARCHITECTURE.md grows from the owner's questions: each answer about how or why the code works is written there, not left in conversation. The first: why execution halts in the anchor under self-stop. |
| 2026-10-19 | A frame source opens each connection by describing its run (a `SourceHello`: seed and frame count), so the socket carries two kinds of message (`std::variant`). Without it the viewer can't know the seed, and a replay address needs it. The context bar draws the last *published* context, so it can't claim what a failed write didn't deliver; changes the user didn't make are published at most ten times a second. |
| 2026-10-19 | Fillscope is also a way to learn modern C++ from the inside (charter §1): its code, the AI-written parts included, is written to be learned from, and the assistant explains running code from the debugger's published state. Prompted by stepping through the frame source, where a variable's value before its lifetime began needed explaining. |
| 2026-10-19 | Stand-ins are how the work bootstraps before the domain is designed: the stand-in frame gains an `id`, a recurring identity to follow, explicitly not a draft of the message interfaces. Order: the debugger-state extension, then the shell's context capture. The capture is tested by the owner too, pointing at and following things in the viewer and asking the assistant questions about them. |
| 2026-10-19 | The context the viewer captures for the assistant: two ways to assemble it from the start, **point at** (one thing) and **follow** (one identity across frames), in separate sections; **published eagerly** on every change, since the question is asked in another program; the context bar renders exactly what is published; the context includes a **replay address**, so the state can be rebuilt exactly; written to `.fillscope/context.json`, git-ignored, atomically, with a sequence number and timestamp. |
| 2026-10-19 | CI: GitHub Actions in a Fedora container, running every test in both builds and the checks of the tests; debugger tests fail there rather than skip. Publishing the debugger's state comes before the viewer shell. |
| 2026-10-19 | Plumbing starts now, before the domain reading is done: the processes, the frame channel, the animation and debugger lockstep, starting with the experiment in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md). The owner reads the domain in parallel. |
| 2026-10-19 | Several processes and threads, not one: the parts communicate over sockets, so the system shows networking and concurrency. The split is decided as the design develops. |
| 2026-10-19 | Linux is the platform for everything, the UI included (was: the UI's platform being settled). |
| 2026-10-19 | Who writes what: the owner hand-writes the code that demonstrates modern C++ design, decided stage by stage as the owner learns each stage's domain (the owner is new to it); Claude writes the rest. Replaces "the core (book, matching, replay, fill models) is hand-written". |
| 2026-10-19 | Claude tests all the code aggressively, the owner's included, so that later stages are never built on undetected defects; the owner writes some tests too. Tasking AI with tests is itself one of the judgements Fillscope demonstrates. |
| 2026-10-19 | Polish: as much as compelling demo videos need, and no more (was "polish, last"). |
| 2026-10-19 | Fillscope also demonstrates judgement in using AI to write code: AI output is trusted only through a check independent of the AI, and each use is documented with its check (charter §1, decision 7). |
| 2026-10-19 | MIT license. |
| 2026-10-19 | Repository created, public from the first commit. Tagline: "An explainable order-book backtester: click any order, ask why it filled." |
| 2026-10-16 | The name Fillscope: "fill" is the trading term for an executed order, and "scope" an instrument for looking inside. No product or repository with the name was found (checked 2026-10-16 and 2026-10-19). |
| 2026-10-16 | An observatory for its own simulator and backtester rather than one more order book; no AI inside the app. |
