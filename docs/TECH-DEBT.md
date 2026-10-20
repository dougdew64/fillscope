# Tech debt backlog

**Purpose:** known defects, test gaps and other debt, each with its evidence and what would fix it.
**Status:** live plan; an item is removed when fixed (git keeps it), and the file is deleted when
empty. **Read when:** choosing what to fix, or before changing code an item names.

Two sweeps have been made, both on 2026-10-19, each by four independent reviews and a pass over
the documents; every item was reproduced or confirmed in the code before it was recorded. The
first found 59 items, fixed in the "Tech debt, chunk 1" to "chunk 5" commits. The second, over all
the code, found 64, fixed in the "Second sweep, chunk 1" to "chunk 5" commits; their messages name
the items each fixed. What remains is below.

## For the owner
1. **The charter's decision 3** says the app "logs the context of what is selected"; it writes a
   file, `.fillscope/context.json`, replaced on every change (as README.md says).
2. **README.md** lacks the Purpose, Status and Read when lines that CLAUDE.md requires of every
   document.

## Not tested, and why
3. **The anchor's warning when `/proc/self/status` can't be read** has no test: a test can't make
   that file unreadable to its own process without a container of its own.
4. **The LLDB driver's guard against a runaway run** ("too many stops", after 1,000) never runs:
   no test program stops that often.

## Left in place deliberately
5. **The debug-state file shows one stop per session: the latest.** In all-stop debugging, which
   GDB and LLDB use here, that is the whole truth. In non-stop debugging, another thread can still
   be stopped after the published one resumes. *Fix, if non-stop debugging is ever used:* publish
   stops per thread.
6. **Two VS Code windows on one Fillscope folder overwrite each other's debug-state file.** The
   file names its writer's process id, and the documents say to use one window.
7. **A writer that crashes, or is killed with SIGKILL, leaves its file saying "running".** The
   viewer handles SIGINT and SIGTERM, and the extension marks its last write; for the rest, both
   files name their writer's process id and start time, and a reader that must be sure checks
   that the process is alive.
8. **CI's Fedora packages aren't pinned:** the repositories keep only the newest update of each,
   so each run records the toolchain's versions instead.
