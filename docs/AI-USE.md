# How AI is used here, and what checks it

**Purpose:** each use of AI to write code in Fillscope, what checks its output, and the defects
those checks caught (charter decision 7). **Status:** record. **Read when:** judging how far the
AI-built parts can be trusted, or adding a new use.

The rule: AI output is trusted only through a check that doesn't depend on the AI. Code written by
hand is listed nowhere here; commits co-authored by Claude carry a `Co-Authored-By` trailer.

## Uses
| What | Written by | Checked by |
|---|---|---|
| The channel, the debugger anchor, the viewer and the stand-in frame source (`src/`, `tools/`; [RUNNING.md](RUNNING.md)), begun as the lockstep experiment | Claude | Their tests, in a normal and a sanitizer build; a mutation check (14 deliberate bugs, each must fail a named test); the owner, stepping them in VS Code |
| The tests (`tests/`) | Claude | The mutation check (a test that can't fail is caught there); each new test seen to fail before its fix |
| The debug-state extension (`tools/vscode-debug-state/`) | Claude | Its unit tests, driving it with fake debug sessions; a type check of its JavaScript by the TypeScript compiler, shown to catch a misspelt property the unit tests miss; four mutants; the owner, stepping with it in VS Code (pending) |
| Test selection (`tests/scripts/iterate.py`) and tiers | Claude | `selection_check.py` (12 changes, each must select exactly the right programs), itself shown to fail when the selection ignores run-time files; a mutant that makes a unit test slow, which must fail on its timeout |

## Defects caught
Newest first. "Who caught it" is the check, not the author of the fix.

| Date | Defect | Who caught it |
|---|---|---|
| 2026-10-19 | A test run by a command (Node) has no program, so its manifest line was one field short, and the sanitizer check would have treated a `.js` file as a program | Claude, reading the manifest before the full run |
| 2026-10-19 | The document link test also listed the README files inside `node_modules` | Claude, from the test's document count (15, not 8) |
| 2026-10-19 | The mutant meant to recreate the `atomic_base.h` bug used `std::atomic_ref`, which doesn't reproduce it; the test was fine and the mutant wasn't | The mutation check (reported it as not caught) |
| 2026-10-19 | A viewer test checked for the absence of "last frame:", which "seconds since last frame:" contains | The test, failing on correct code |
| 2026-10-19 | A debugger test expected LLDB to name the anchor `fillscope_frame_anchor`; it says `::fillscope_frame_anchor(uint64_t)` | The test, failing on correct code |
| 2026-10-19 | A breakpoint on the anchor by name opened `atomic_base.h` under LLDB: the anchor's first statement was an `std::atomic` store, inlined from the header | **The owner**, in VS Code. Claude's own LLDB output had shown it and was misread. Now guarded by a debugger test and a mutant |
| 2026-10-19 | The viewer wrote `imgui.ini` into the working directory | Claude, from `git status` after a run |
| 2026-10-19 | `raise(SIGTRAP)` as the self-stop stopped two frames down inside libc under GDB | The experiment's GDB run |
