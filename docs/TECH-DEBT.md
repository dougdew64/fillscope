# Tech debt backlog

**Purpose:** known defects, test gaps and other debt, each with its evidence and what would fix it.
**Status:** live plan; an item is removed when fixed (git keeps it), and the file is deleted when
empty. **Read when:** choosing what to fix, or before changing code an item names.

The first sweep (2026-10-19: four independent reviews and a pass over the documents) found 59
items. All were fixed in four commits, "Tech debt, chunk 1" to "chunk 4", plus the documents and
CI that followed; the commit messages name the items each fixed. What remains are limitations
that were left in place deliberately, each stated where a reader would meet it.

1. **The debug-state file shows one stop per session: the latest.** In all-stop debugging, which
   GDB and LLDB use here, that is the whole truth. In non-stop debugging, another thread can still
   be stopped after the published one resumes, and the file would say "running". *Fix, if
   non-stop debugging is ever used:* publish stops per thread.
2. **Two VS Code windows on one Fillscope folder overwrite each other's debug-state file.** The
   file names its writer's process id, so a reader can tell, and RUNNING.md and
   DEBUGGER-LOCKSTEP.md say to use one window. *Fix, if it matters:* a file per window.
3. **A writer that crashes leaves its file saying "running".** Both published files name their
   writer's process id and start time; a reader that must be sure checks that the process is
   alive. An exit, normal or by VS Code shutting the extension down, marks the writer stopped.
4. **CI's Fedora packages aren't pinned.** The image is pinned by digest, but Fedora's
   repositories keep only the newest update of each package, so an exact version can't be
   installed later; each run records the toolchain's versions instead.
