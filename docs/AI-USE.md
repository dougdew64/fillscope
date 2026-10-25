# How AI is used here, and what checks it

**Purpose:** each use of AI to write code in Fillscope, what checks its output, and the defects
those checks caught (charter decision 6). **Status:** record. **Read when:** judging how far the
AI-built parts can be trusted, or adding a new use.

The rule: AI output is trusted only through a check that doesn't depend on the AI. Code written by
hand is listed nowhere here.

**Attribution is mechanical.** A commit is either the owner's, with no trailer, or co-authored by
Claude, with a `Co-Authored-By: Claude` trailer; never both. A change Claude suggests to the
owner's code is typed by the owner, or committed on its own, co-authored. Which code is the owner's
is decided stage by stage ([charter](CHARTER.md) §4), never in advance. So `git blame` can say who
wrote every line, and [ATTRIBUTION.md](ATTRIBUTION.md) says it per file, generated from the
history by `tools/attribution/attribution.py`, which anyone can rerun to check it. The back end's
fork has its own report, `backend/ATTRIBUTION.md`, with a third kind of line: upstream's, the book's
code as Packt's repository has it.

## Uses
| What | Written by | Checked by |
|---|---|---|
| The shared helpers (`src/common/`); before them, the stand-ins the lockstep experiment began with (a frame source, its socket and its debugger anchor, retired on 2026-10-23) | Claude | `common_tests`, in a normal and a sanitizer build; mutants, each caught by a named test; the stand-ins had their own tests and mutants, and the owner stepped them in VS Code |
| Fillscope's reading of the exchange's market data (`src/feed/`) and the viewer of its books (`src/viewer/`) | Claude | `feed_tests` and `viewer_tests`; the UI tests, with real clicks and real multicast; `feed_system_tests`, which checks the books against the real exchange's own snapshots, a late reader's recovery, and the run's recording replayed against what arrived live, byte for byte; `replay_debugger_tests`, lockstep on a replay under GDB and LLDB; mutants of the decoding, the books, recovery, the context and the view; the owner, watching it live |
| The back end's changes (`backend/`, the fork; its README lists them): deterministic replay, the lockstep anchor, the price level table, the bug fixes, the options | Claude | The fork's own tests (`lowlatency_tests`), in a normal and a sanitizer build and in the fork's CI; `feed_system_tests`, the replay byte for byte against a live run; `replay_debugger_tests`, under GDB and LLDB; `backend_system_tests`; mutants of the matcher, the price levels, the market data, the recording, the replay, the anchor and the command lines |
| The answer tool (`src/answer/`), and the answers it makes: the assistant's explanations, as scenarios of requests with claims ([ARCHITECTURE.md](ARCHITECTURE.md), §10) | Claude | Each answer by the real matcher: its rows are a replay of its requests through `exchange_replay`, never written by the AI, and every claim it states is checked against that replay, a false one refused before it is shown. The tool by `answer_tests` and `answer_system_tests` (the latter on real replays, pinning down how the matcher matches as an answer will say it); mutants of each rule, each caught |
| The tests (`tests/`) | Claude | The mutation check (a test that can't fail is caught there); each new test seen to fail before its fix |
| The debug-state extension (`tools/vscode-debug-state/`) | Claude | Its unit tests, driving it with fake debug sessions; a type check of its JavaScript by the TypeScript compiler, shown to catch a misspelt property the unit tests miss; mutants, each naming the single tests that catch it; the owner, stepping with it in VS Code while the assistant read the file (2026-10-19) |
| The attribution report (`tools/attribution/`, [ATTRIBUTION.md](ATTRIBUTION.md)) | Claude | `attribution_tests`, on small repositories built for each case (trailers, whitespace-only changes, moved lines, binary files); a check, run every time, that the committed report is exactly what its named commit gives; mutants of each rule, each caught; and anyone, by rerunning it |
| The code map's check (`tools/code_map/`) | Claude | `code_map_tests`, on small repositories built for each case (a submodule, untracked, ignored and deleted files); a check, run every time, of the committed [CODE-MAP.md](CODE-MAP.md); mutants of each rule, each caught |
| Test selection (`tests/scripts/iterate.py`), tiers and the checks of the tests, and the mutation check's choice of mutants | Claude | `scripts_tests`, the scripts' own unit tests; `selection_check.py` (changes, each must select exactly the right programs, and runs that must run, record and not record the right ones; and changes, each must make the mutation check choose exactly the right mutants), itself shown to fail when the selection ignores run-time files or the files that always run; mutants of the scripts' logic and of each tier's timeout |

## Defects caught
Newest first. "Who caught it" is the check, not the author of the fix. Where it is **the owner**,
none of the AI's checks had: those rows are the episodes worth rereading.

| Date | Defect | Who caught it |
|---|---|---|
| 2026-10-25 | The answer tool's tests of a `trades` claim had only buys crossing, so a checker that took every trade's aggressor as the buyer passed them: an answer could have called a sell crossing the bids a buy, and been checked true | The mutation check: the mutant "a trade's aggressor is taken as the buyer" wasn't caught. A test of a sell crossing now catches it |
| 2026-10-24 | The book's `ASSERT` was a function taking a `std::string`, so every call built its message, and one longer than a string's own 15-character buffer allocated: on every message read from a queue, every allocation from a memory pool, and every update to a snapshot, in code whose pools exist so that trading never allocates. Claude had written three such calls itself (`PriceLevelTable`, every price level added or removed), and the same day wrote the full-queue check as an `if` around `FATAL` for exactly this reason, without looking at the other 57 calls | **The owner**, studying the book's code, asked whether `ASSERT` is a macro. Measured: 1,000 queue reads made 1,000 allocations, 1,000 pool round trips 3,000, at `-O0` and `-O2`. Now a macro that evaluates its message only on failure; `lowlatency_allocation_tests`, which counts every `operator new`, failed before the fix |
| 2026-10-19 | Python bytecode caches (`__pycache__`), written when the scripts loaded one another by path, were committed in "Second sweep, chunk 4" (with the absolute path of the working tree) | The commit's file list, read after pushing. Removed in the next commit; the scripts no longer write them and `__pycache__/` is ignored |
| 2026-10-19 | While fixing the second sweep: a test asserting a spike survives every reduction was wrong at width 1, where everything merges; mutants showed several new tests didn't test what they claimed (a run's number only distinguishes runs whose frames are equal; an equivalent mutant of the self-stop's nop; a text field's fallback) | The tests failing on correct code, and the mutation check, each time before the work was committed |
| 2026-10-19 | The second sweep's 64 items, over all the code, after the first sweep's fixes: a viewer stopped by a signal still claiming to run, a run wiped by an empty connection, the test checks overwriting edits, a program with no tests recorded as passed, and more (fixed in the "Second sweep" commits; [TECH-DEBT.md](TECH-DEBT.md) keeps what remains) | Four independent AI reviews and a document pass. Every item reproduced or confirmed in the code; one reported defect, reproduced by its reviewer, was false and was dropped |
| 2026-10-19 | Turning on warnings as errors for CI would have made five mutants fail to compile (deliberately broken code draws warnings), so the mutation check would have failed on every CI run | Running CI's configuration locally before pushing. `mutants.py` now builds mutants with warnings allowed and restores the setting |
| 2026-10-19 | Changing the anchor's stored index to `std::optional` (and trying `[[gnu::noipa]]`) moved GDB's breakpoint, in the sanitizer build only, to before the argument was stored: every stop read `frame_index` as 0 | `LockstepDebugger.GdbBreakpointByName` in the sanitizer build. The fix: plain stores; the comment and DEBUGGER-LOCKSTEP.md rule 2 record why |
| 2026-10-19 | A new test assumed a listener's queue holds exactly its backlog; Linux holds one more | The test, failing on correct code |
| 2026-10-19 | The first sweep's 59 items: published state that can be false, test machinery that can answer wrongly, crashes, and more (fixed in the "Tech debt" commits; [TECH-DEBT.md](TECH-DEBT.md) keeps what remains) | Four independent AI reviews and a document pass; every item reproduced or confirmed in the code before it was recorded |
| 2026-10-19 | Three UI tests waited a number of frames for things that take real time (the publisher's interval, a tooltip's delay); the Test Engine's fast mode runs those frames in about a millisecond | The tests, failing on correct code |
| 2026-10-19 | A test that waited for messages that by design never arrive took a full second, half its tier's two-second budget | Claude, from the slowest-tests list `run-tests.sh` prints |
| 2026-10-19 | `EXPECT_EQ(x, Message{SourceHello{.seed = 7, .frame_count = 50}})` doesn't compile: a macro's arguments split at commas, and braces don't protect them | clangd, in the editor, before the build |
| 2026-10-19 | A code comment said evaluation order inside `Frame{a(), b()}` is unspecified; braced initializers are evaluated left to right (it is function-call arguments whose order is unspecified) | Claude, rereading it, before it was committed |
| 2026-10-19 | A test run by a command (Node) has no program, so its manifest line was one field short, and the sanitizer check would have treated a `.js` file as a program | Claude, reading the manifest before the full run |
| 2026-10-19 | The document link test also listed the README files inside `node_modules` | Claude, from the test's document count (15, not 8) |
| 2026-10-19 | The mutant meant to recreate the `atomic_base.h` bug used `std::atomic_ref`, which doesn't reproduce it; the test was fine and the mutant wasn't | The mutation check (reported it as not caught) |
| 2026-10-19 | A viewer test checked for the absence of "last frame:", which "seconds since last frame:" contains | The test, failing on correct code |
| 2026-10-19 | A debugger test expected LLDB to name the anchor `fillscope_frame_anchor`; it says `::fillscope_frame_anchor(uint64_t)` | The test, failing on correct code |
| 2026-10-19 | A breakpoint on the anchor by name opened `atomic_base.h` under LLDB: the anchor's first statement was an `std::atomic` store, inlined from the header | **The owner**, in VS Code. Claude's own LLDB output had shown it and was misread. Now guarded by a debugger test and a mutant |
| 2026-10-19 | The viewer wrote `imgui.ini` into the working directory | Claude, from `git status` after a run |
| 2026-10-19 | `raise(SIGTRAP)` as the self-stop stopped two frames down inside libc under GDB | The experiment's GDB run |
