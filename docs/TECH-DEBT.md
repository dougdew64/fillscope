# Tech debt backlog

**Purpose:** known defects, test gaps and other debt, each with its evidence and what would fix it.
**Status:** live plan; an item is removed when fixed (git keeps it), and the file is deleted when
empty. **Read when:** choosing what to fix, or before changing code an item names.

Three sweeps have been made, each by four independent reviews and a pass over the documents;
every item was reproduced or confirmed in the code before it was recorded. The first two, on
2026-10-19, found 59 and 64 items, fixed in the "Tech debt, chunk 1" to "chunk 5" and "Second sweep,
chunk 1" to "chunk 5" commits. The third, on 2026-10-23, over Fillscope and the fork after the back
end arrived, is fixed in the "Third sweep" commits, here and in the fork; their messages name the
items each fixed. What remains is below.

The back end's known issues (the fork of the book's trading system, `backend/`) are listed in the
"Known issues" of its README, `backend/README.md`, which owns them; the fork fixes its bugs as it
finds them, each with a test, and lists them there.

## Not tested, and why
1. **The request anchor's warning when `/proc/self/status` can't be read** (the back end's
   `lockstep_anchor.cpp`) has no test: a test can't make that file unreadable to its own process
   without a container of its own.
2. **The replay's LLDB driver's guard against a runaway run** (`tests/backend/replay_lldb_driver.py`,
   after 1,000 stops) never runs: no test replays that many requests.

3. **The machinery's own helpers** are tested only through the checks that use them:
   `scratch_tree.py` (the copy, its submodule, the borrowed history), the mutation check's reading
   of each runner's output, and `iterate.py`'s `--snapshot` and `--record-snapshot`, which only
   `run-tests.sh` uses. A wrong one shows as a check failing, or as one passing that shouldn't.
4. **The selection check can't see the back end's build files go unnoticed:** its copy flattens the
   submodule into plain files, which git lists either way. `iterate.py` now asks the submodule too.

## Left in place deliberately
5. **A run with a failing test records no program as passed,** so the next run repeats the ones
   that passed too. That costs at most a run of every fast test program, about 2.3 seconds
   (measured 2026-10-24); recording the ones that passed would mean reading each program's result
   out of CTest's output, in the code that decides which tests to skip, where a mistake would skip
   a test silently. The owner chose simplicity and confidence in the results (DECISIONS.md).
6. **The debug-state file shows one stop per session: the latest.** In all-stop debugging, which
   GDB and LLDB use here, that is the whole truth. In non-stop debugging, another thread can still
   be stopped after the published one resumes. *Fix, if non-stop debugging is ever used:* publish
   stops per thread.
7. **Two VS Code windows on one Fillscope folder overwrite each other's debug-state file.** The
   file names its writer's process id, and the documents say to use one window.
8. **A writer that crashes, or is killed with SIGKILL, leaves its file saying "running".** The
   viewer handles SIGINT and SIGTERM, and the extension marks its last write; for the rest, both
   files name their writer's process id and start time, and a reader that must be sure checks
   that the process is alive.
9. **CI's Fedora packages aren't pinned:** the repositories keep only the newest update of each,
   so each run records the toolchain's versions instead.
