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

## Waiting on the owner's decision
Each has a real trade-off; until decided, the code does what is described.
5. **Requests the exchange can't process, live.** The matcher indexes its tables by a request's
   ids unchecked, so a bad request is undefined behaviour or ends the exchange, and the trading
   client's order ids, which only increase, end it after 1,048,576 orders (the fork's README, known
   issues). A recording is now checked when read. *Choices:* reject such a request in the order
   server (a change to the book's behaviour), skip it, or keep the book's behaviour.
6. **A recording that can't be written** stops, says so on stderr, and the exchange runs on.
   *Choices:* stop the exchange (a recording is then always whole, at the cost of the run), or run
   on with a marker a replay can see (a trailer, or a file beside it).
7. **A full lock-free queue is overwritten** (the fork's README). *Choices:* stop the program when
   a queue is full (an assertion; the run ends), or block the writer (changes the latency the book
   measures), or keep overwriting.
8. **Market data leaves the machine** (the fork's README). Keeping it on loopback means choosing the
   interface in the publisher and every listener, and turning multicast on for the loopback
   interface, which needs root.
9. **CI on main can skip a commit:** of three pushes while a run is going, the middle one gets no
   result of its own (`ci.yml` says why). *Choice:* a concurrency group per commit gives each its
   own run, in parallel, using more runner time.
10. **Separate runs of the tests can meet on the network.** CTest's lock keeps the tests that start
    an exchange apart within one run, but the mutation and selection checks, a second `run-tests.sh`
    and a lockstep session in VS Code run separately, and the groups and ports are fixed, so their
    datagrams can reach each other; `feed_system_tests` and `backend_system_tests` now fail at once
    if an exchange is already running. *Choice:* a lock for the whole machine, taken by every
    network test, which would serialize the checks that run in parallel today.
11. **A run with a failing test records no program as passed,** so the next run repeats the ones
    that passed too. *Choice:* record those that passed (reading each program's result from
    CTest's output), which is fewer re-runs for more logic to get right.

## Left in place deliberately
12. **The debug-state file shows one stop per session: the latest.** In all-stop debugging, which
   GDB and LLDB use here, that is the whole truth. In non-stop debugging, another thread can still
   be stopped after the published one resumes. *Fix, if non-stop debugging is ever used:* publish
   stops per thread.
13. **Two VS Code windows on one Fillscope folder overwrite each other's debug-state file.** The
   file names its writer's process id, and the documents say to use one window.
14. **A writer that crashes, or is killed with SIGKILL, leaves its file saying "running".** The
   viewer handles SIGINT and SIGTERM, and the extension marks its last write; for the rest, both
   files name their writer's process id and start time, and a reader that must be sure checks
   that the process is alive.
15. **CI's Fedora packages aren't pinned:** the repositories keep only the newest update of each,
   so each run records the toolchain's versions instead.
