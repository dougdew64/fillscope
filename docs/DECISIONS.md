# Decisions: the record

**Purpose:** what was decided, when and why, including what was later reversed.
**Status:** record; binds nothing (the current answer is in the [charter](CHARTER.md) or the code).
**Read when:** wondering why something is the way it is.

Newest first.

| Date | Decision |
|---|---|
| 2026-10-19 | Who writes what: the owner hand-writes the code that demonstrates modern C++ design, decided stage by stage as the owner learns each stage's domain (the owner is new to it); Claude writes the rest. Replaces "the core (book, matching, replay, fill models) is hand-written". |
| 2026-10-19 | Claude tests all the code aggressively, the owner's included, so that later stages are never built on undetected defects; the owner writes some tests too. Tasking AI with tests is itself one of the judgements Fillscope demonstrates. |
| 2026-10-19 | Polish: as much as compelling demo videos need, and no more (was "polish, last"). |
| 2026-10-19 | Fillscope also demonstrates judgement in using AI to write code: AI output is trusted only through a check independent of the AI, and each use is documented with its check (charter §1, decision 7). |
| 2026-10-19 | MIT license. |
| 2026-10-19 | Repository created, public from the first commit. Tagline: "An explainable order-book backtester: click any order, ask why it filled." |
| 2026-10-16 | The name Fillscope: "fill" is the trading term for an executed order, and "scope" an instrument for looking inside. No product or repository with the name was found (checked 2026-10-16 and 2026-10-19). |
| 2026-10-16 | An observatory for its own simulator and backtester rather than one more order book; no AI inside the app. |
