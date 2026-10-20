# How AI is used here, and what checks it

**Purpose:** each use of AI to write code in Fillscope, what checks its output, and the defects
those checks caught (charter decision 7). **Status:** record. **Read when:** judging how far the
AI-built parts can be trusted, or adding a new use.

The rule: AI output is trusted only through a check that doesn't depend on the AI. Code written by
hand is listed nowhere here; commits co-authored by Claude carry a `Co-Authored-By` trailer.

## Uses
| What | Written by | Checked by |
|---|---|---|
| The channel, the debugger anchor, the viewer and the stand-in frame source (`src/`, `tools/`; [RUNNING.md](RUNNING.md)), begun as the lockstep experiment | Claude | Their tests, in a normal and a sanitizer build; a mutation check (deliberate bugs, each must fail a named test); the owner, stepping them in VS Code |
| The tests (`tests/`) | Claude | The mutation check (a test that can't fail is caught there); each new test seen to fail before its fix |
| The debug-state extension (`tools/vscode-debug-state/`) | Claude | Its unit tests, driving it with fake debug sessions; a type check of its JavaScript by the TypeScript compiler, shown to catch a misspelt property the unit tests miss; mutants, each naming the single tests that catch it; the owner, stepping with it in VS Code while the assistant read the file (2026-10-19) |
| Test selection (`tests/scripts/iterate.py`), tiers and the checks of the tests | Claude | `scripts_tests`, the scripts' own unit tests; `selection_check.py` (changes, each must select exactly the right programs, and runs that must run, record and not record the right ones), itself shown to fail when the selection ignores run-time files or the files that always run; mutants of the scripts' logic and of each tier's timeout |

## Defects caught
Newest first. "Who caught it" is the check, not the author of the fix.

| Date | Defect | Who caught it |
|---|---|---|
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
