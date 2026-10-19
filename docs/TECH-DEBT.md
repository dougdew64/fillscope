# Tech debt backlog

**Purpose:** known defects, test gaps and other debt, ranked, each with its evidence and a fix.
**Status:** live plan; an item is removed when fixed (git keeps it), and the file is deleted when
empty. **Read when:** choosing what to fix, or before changing code an item names.

From the second sweep, 2026-10-19, over all the code (not only what changed since the first):
four independent reviews (the viewer and the channel; the anchor and the frame source; the
debug-state extension; the build, test machinery and CI), plus a pass over the documents. Every
item was verified. **Reproduced** means it was shown happening, with a probe or the real program.
**Read** means it was confirmed in the code at the place named. One reported defect was false (a
`--interval-ms` overflow: the value is parsed as 32 bits and a larger one is refused) and is not
listed. The first sweep's 59 items were fixed in the "Tech debt, chunk 1" to "chunk 5" commits.

## A. Published state that can be false
1. **A viewer stopped by a signal leaves its file saying "running"** (reproduced). `main.cpp`
   handles no signals, so Ctrl-C, SIGTERM, and VS Code's Stop end the viewer without `finish()`.
   These are ordinary ways to stop it, not crashes. *Fix:* SIGINT and SIGTERM set a flag that ends
   the loop; test with a child viewer sent SIGTERM.
2. **The extension can describe a normal end as an adapter exit** (read; VS Code's call order is
   unconfirmed). If VS Code calls `onExit(0)` without `onWillStopSession` when a program ends by
   itself, the entry says `endReason: "the debug adapter exited (code 0, signal none)"`, which
   reads as a crash. *Fix:* exit code 0 with no signal is a normal end; also end sessions on
   `vscode.debug.onDidTerminateDebugSession`, which fires for every kind of adapter (item 15).
   Check the real order in VS Code.
3. **"N of M frames" goes wrong when a source sends more frames than its hello said** (read;
   `context.cpp`, `bar_text`). Hello `frame_count 3` then 5 frames shows "5 of 3 frames", and the
   file has no flag for the broken promise. Latent: the frame source keeps it. *Fix:* say "5 frames
   (the source said 3)"; test it.
4. **A replay address assumes the run's indices are 0, 1, 2...** (read; `make_context`). Nothing
   checks this. A gap or a repeated index gives an address that replays a different frame. A
   repeated index also appears twice in a follow list. Latent: the frame source never does this.
   *Fix:* on a gap or a repeat, drop replay addresses for the rest of the run, with the reason;
   test it.
5. **The price plot rounds prices above 2^24** (read; `viewer_state.cpp`,
   `static_cast<float>(frame.price)`). 16,777,217 and 16,777,216 plot as equal, so "no spike is
   lost" (ARCHITECTURE.md) fails for large prices. Latent: stand-in prices are near 10,000. *Fix:*
   keep the envelope in `int64_t`, and convert only the drawn points, offset by the run's minimum.
6. **The extension can read an expensive scope** (read; `describeStop`). It takes the first scope
   whose name matches `/local/i`, even one marked expensive or named "Thread Locals". That
   contradicts DEBUGGER-LOCKSTEP.md: "expensive scopes are never read". *Fix:* prefer the scope
   with `presentationHint: 'locals'`, then the cheapest whose name is exactly "Locals" or "Local",
   and never an expensive one; test each.
7. **The frame source reports success to a viewer that never took its frames** (reproduced). A
   second source waits in the listener's queue, and its sends succeed into the socket buffer: run
   against a listener that never accepts, it prints "sent 5 frames" and exits 0. Under the
   debugger, such a source stops at the anchor for frames the viewer never shows. *Fix:* the viewer
   closes queued connections it won't accept, or the docs and the message say the frames were
   only queued.

## B. Losing the user's work
8. **A connection that sends nothing valid wipes the run being examined** (read; `ViewerApp::receive`
   calls `begin_connection()` on any new sender). A connection that sends garbage, or nothing,
   discards the previous, complete run and the point. *Fix:* start the new connection's run at its
   first valid message; test with a garbage-only connection and an empty one.
9. **The checks of the tests overwrite edits made while they run** (read; reproduced by the
   reviewer). `mutants.py` and `selection_check.py` restore the source they captured before
   mutating it, so saving that file during the run loses the edit. They also build in `build/debug`,
   the working build, which the editor's build may be using at the same moment. *Fix:* run both in
   a temporary `git worktree`, never in the working tree.

## C. Test machinery that can answer wrongly
10. **A program with no tests is recorded as passed** (read; reproduced by the reviewer). A
    manifest program with no tests in CTest passes `--check-registration`, and `all_skipped`
    ignores it (`if tests and ...`). For example, every test renamed `DISABLED_` is recorded as
    passed with nothing tested. *Fix:* registration requires at least one test per program; a
    program with none is never recorded.
11. **`mutants.py` ignores a failed restore of the warnings setting** (read). The `finally`
    ignores `set_werror`'s result, so a failed reconfigure leaves `build/debug` without
    `-Werror`, silently. Moving the check to a worktree (item 9) removes this too.
12. **`npm ci` reruns on every full run** (reproduced: "added 5 packages" in the sanitizer build
    with nothing changed). Both builds declare the same output in the source tree, so each sees
    the other's install as newer. Harmless under the lock, but slow and noisy. *Fix:* one stamp
    per build directory, keyed by the lock file.

## D. Input refused late, or not at all
13. **An empty `--socket ""` is accepted** (reproduced). The viewer binds a name Linux chooses
    (autobind), which no source can know; the frame source retries for seconds before failing.
    *Fix:* both parsers refuse it, with a test.
14. **`--exit-after inf` is accepted and never exits** (reproduced), though 0 already means
    "never". *Fix:* refuse a value that isn't finite; test it.
15. **The extension never listens for `onDidTerminateDebugSession`** (read). An adapter running
    as a server, or inline, may end a session without `onExit`, and its entry then stays "running"
    or "stopped" for good.
16. **Self-stop says nothing when `/proc/self/status` can't be read** (read; `tracer_attached_now`).
    A failed read means "no tracer", so a debugger session runs freely with no word why. *Fix:*
    warn once on stderr when self-stop is on and the file can't be read.

## E. Test gaps
17. **Nothing checks the anchor at `-O2`** (read). Every build is `-O0`; removing the anchor's
    `asm volatile` clobber passes every test (reproduced by the reviewer). DEBUGGER-LOCKSTEP.md's
    rule 1 was checked once, by hand. *Fix:* a `check`-tier test that compiles the anchor at `-O2`
    and checks with `nm` and `objdump` that it keeps its own address and its store.
18. **The test scripts have no tests of their own** (read; reproduced by the reviewer).
    `check_registration` returning nothing, `all_skipped` returning nothing, or the `libubsan`
    check removed: all pass CI. `selection_check.py` never checks that a failed run is not
    recorded, or that the slow tier isn't run. *Fix:* a Python `unittest` program in the `unit`
    tier, covering parsing, `all_skipped`, registration and the pass record; and a run case with a
    test made to fail.
19. **Viewer** (mutants that survive, reproduced by the reviewer):
    - nothing checks the source's error through `ViewerApp`; deleting `source_error = receiver.error()` passes;
    - nothing checks `ViewerApp::finish`;
    - nothing checks "seconds since last frame"'s value, only that it appears;
    - nothing checks the plot's points; dropping each bucket's high passes. A pure `plot_points` function would let a unit test check them;
    - nothing checks the envelope's frame count across merges; changing `+=` to `=` in the even branch passes;
    - nothing checks that the table draws the followed rows' colour (Dear ImGui doesn't expose it), yet RUNNING.md says the UI tests check the highlights.
20. **The frame source and anchor:**
    - Nothing checks that `--interval-ms` is honoured; removing the sleep passes every test.
    - A `set_self_stop` that does nothing is caught only by the debugger tier.
    - `lldb_self_stop.py`'s failure branches never run.
21. **The extension:**
    - No test of `onExit(0)` after a program's own exit (item 2), or of `onError`.
    - The concurrent-reader test doesn't assert that its reads overlapped the writes, so it can pass without testing anything on another machine.
    - `extension.test.js` waits a fixed 20 ms for asynchronous work.
    - A stop going stale while members are being read is untested.
22. **Mutants name whole programs:** 22 of 60 name `viewer_ui_tests` or
    `vscode_debug_state_tests`, so nothing makes sure the intended test is the one that fails. The
    reviewer checked all 22, and each was caught by its intended test. *Fix:* name single tests,
    through Node's `--test-name-pattern` and the Test Engine's filter.

## F. Code quality
23. **Viewer:**
    - `ViewerApp::receive` allocates a new vector every paint;
    - `bar_text` formats its strings every paint, though the published context rarely changes;
    - the plot allocates two vectors every paint;
    - `Run` keeps every frame, so memory grows with the run (about 32 bytes a frame, so 10 million frames is 320 MB). State the bound, or cap the run.
24. **Anchor and frame source:**
    - with self-stop on, the anchor reads `/proc/self/status` on every call, through `ifstream` and `ostringstream`, which Step Into walks through;
    - `last_sent` in `run_frame_source` duplicates `options.frames > 0`.
25. **Extension:**
    - `extension.js` duplicates `messageOf`;
    - `sessionEnded` builds a fake session to call `change`;
    - pending resumes are keyed `"id#seq"`, so ending a session scans every key;
    - two workspace folders reaching one directory through a symlink get two publishers writing one file (key by `realpath`).
26. **Test machinery:**
    - `iterate.py` lists CTest's tests twice, with the same parsing code;
    - `fingerprints()` reuses the name `shared` for a hash object and then for a string;
    - `fillscope_gtest` re-parses two options only to pass them on.

## G. Documents
27. **ARCHITECTURE.md:**
    - says the `static_assert`s check that the payloads are trivially copyable; they check `has_unique_object_representations`, which also rules out padding;
    - "no spike is lost" (item 5);
    - "SourceHello opens every connection" overclaims, also in `message.h`: the viewer accepts connections without one, and tests rely on it.
28. **DEBUGGER-LOCKSTEP.md:**
    - cites `Anchor.SelfStopWithoutADebuggerDoesNotStop`; the test is `Anchor.SelfStopWithoutATracerDoesNotStop`;
    - rule 1's "Checked in the `-O2` build" reads as a standing check (item 17);
    - "expensive scopes are never read" (item 6);
    - an adapter crash's `endReason` (item 2).
29. **RUNNING.md:**
    - "Each Continue adds one point": each frame adds two plotted points (a bucket's low and high), and after 4,096 frames, buckets merge;
    - "Every form builds first" isn't true of the forms for scripts;
    - "the table's highlights" are tested only through `row_marks` (item 19);
    - "Both always restore the source, even when stopped by a signal" isn't true of SIGKILL, or of an edit made during the run (item 9).
30. **AI-USE.md:**
    - the extension's row says "four mutants" (there are 17);
    - the selection row doesn't say `selection_check.py` now also runs and records.
31. **`extension.js`** says `onExit` is "the only word that the session is over" after a crash; it
    also fires after every normal end.
32. **For the owner:**
    - the charter's decision 3 says the app "logs the context of what is selected"; it writes a file, replaced on every change, as README.md now says;
    - README.md lacks the Purpose / Status / Read when lines that CLAUDE.md requires of every document.

## Left in place deliberately
33. **The debug-state file shows one stop per session: the latest.** In all-stop debugging, which
    GDB and LLDB use here, that is the whole truth. In non-stop debugging, another thread can still
    be stopped after the published one resumes. *Fix, if non-stop debugging is ever used:* publish
    stops per thread.
34. **Two VS Code windows on one Fillscope folder overwrite each other's debug-state file.** The
    file names its writer's process id, and the documents say to use one window.
35. **A writer that crashes leaves its file saying "running"** (a signal it can handle is item 1).
    Both files name their writer's process id and start time; a reader that must be sure checks
    that the process is alive.
36. **CI's Fedora packages aren't pinned:** the repositories keep only the newest update of each,
    so each run records the toolchain's versions instead.
