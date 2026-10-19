# Tech debt backlog

**Purpose:** known defects, test gaps and other debt, ranked, each with its evidence and a fix.
**Status:** live plan; an item is removed when fixed (git keeps it), and the file is deleted when
empty. **Read when:** choosing what to fix, or before changing code an item names.

From the second sweep, 2026-10-19, over all the code (not only what changed since the first):
four independent reviews (the viewer and the channel; the anchor and the frame source; the
debug-state extension; the build, test machinery and CI), plus a pass over the documents. Every
item was verified. **Reproduced** means it was shown happening, with a probe or the real program.
**Read** means it was confirmed in the code at the place named. **Reproduced by the reviewer**
means a reviewer's probe showed it and the code (or the protocol's text) was then checked at the
place named. One reported defect was false (a
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
   `vscode.debug.onDidTerminateDebugSession`, which fires for every kind of adapter (item 33).
   Check the real order in VS Code. VS Code fires `onExit` only for an adapter it runs as a
   process; CodeLLDB 1.12.3, this project's debugger, runs inline, so `onExit` never fires here
   and the "adapter crashed" handling can't happen in this setup. VS Code also never passes a
   signal, so "signal none" is a claim it can't support: a crash killed by a signal reads "code
   none, signal none".
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

8. **`terminated` is published as "exited"** (reproduced by the reviewer; the protocol, at
   `TerminatedEvent`: debugging has ended, which "does **not** mean that the debuggee itself has
   exited"). A disconnected attach session shows an exited program that is still running. *Fix:*
   "exited" only on the `exited` event; leave the state until the session ends, or publish a
   separate "debugging ended".
9. **A stop still being read is lost when another thread resumes alone** (reproduced by the
   reviewer). The thread test (`isAnotherThread`) reads the *published* entry, which during the
   read is still the old one. So `continued { threadId: 2, allThreadsContinued: false }` makes
   thread 1's stop stale, and the file says "running" while thread 1 is at a breakpoint. *Fix:*
   keep the stop being read, with its thread, and test against that.
10. **`continue`'s response is not read for `allThreadsContinued: false`** (reproduced by the
   reviewer; the protocol, at `ContinueRequest`). Only the event is. *Fix:* apply the same thread
   test to the response.
11. **A complete stack is published as shortened when `totalFrames` overstates** (reproduced by
   the reviewer). The protocol says a reply with fewer frames than requested "indicates the end
   of the stack", whatever `totalFrames` says. Five frames with `totalFrames: 9` give
   `framesTruncated: true, frameCount: null`, and a test asserts that wrong behaviour. *Fix:*
   complete exactly when fewer frames came back than were asked for; correct the test.
12. **`goto` and `restartFrame` publish "running"** (reproduced by the reviewer; the protocol
   says for both that the adapter "sends the response and then a `stopped` event"). The program
   doesn't run. *Fix:* the request makes the stop stale without publishing "running".
13. **A stop that names no thread shows the first thread's stack without saying so**
   (read). `stop.threadId` stays null, and the thread test then counts no thread as "another", so
   any single-thread resume clears it. *Fix:* publish the thread read (`framesThreadId`) and test
   against it.
14. **A late `onExit` from a restarted session's old adapter ends the new session** (reproduced by
   the reviewer; VS Code 1.142's own code read). After VS Code's emulated restart under the same
   id, the old process's exit arrives through the old tracker and ends the new entry, which then
   ignores everything. Only adapters run as a process are affected, so not CodeLLDB. *Fix:* each
   tracker carries a token taken at `sessionStarted`; calls with a stale token are ignored.
15. **A failed write leaves the previous file claiming its state** (reproduced by the reviewer).
   The extension logs the failure and moves on; the file still says "stopped" and
   `running: true`, also after `stop()`. The viewer's file has the same property (its bar says
   so, the file doesn't). *Fix:* on a failed write, remove the old file, so a reader finds none
   rather than a wrong one; retry later.
16. **Smaller inaccuracies in the extension's file** (reproduced by the reviewer):
    - "running" is published when the tracker is created, before the program is launched, so a failed launch shows "running", then "ended";
    - a stop's `text` (an exception's name, say) is published as its `description`, a different field in the protocol.

17. **The self-stop shows the closing brace, not the line that stops** (reproduced, GDB 17.2 on
   the debug build: `fillscope_frame_anchor (frame_index=0) at anchor.cpp:83`, the `}`). After
   `int3` the program counter is the next instruction, which GCC attributes to the brace. So VS
   Code highlights `}`, and the debug-state file publishes line 83, while the comment marks the
   `int3` line as where "the debugger stops". *Fix (verified by the reviewer under LLDB):*
   `int3` followed by a `nop` on the same line; then check the line (item 37).
18. **The frame source's message is garbled when the viewer goes before the run's description is
   sent** (read; `report_failed_send`). It prints "the viewer went away the run's description":
   one phrase must fit both "went away {}" and "can't send {}". *Fix:* a phrase for each.
19. **At `-O2` the self-stop can't read `frame_index`** (reproduced by the reviewer: GDB shows
   `<optimized out>`, LLDB `None`, and both self-stop debugger tests fail at `-O2`).
   DEBUGGER-LOCKSTEP.md says `frame_index` is readable, which is true at `-O0` only. *Fix
   (verified by the reviewer):* give the `asm` statement `frame_index` as an input, or limit the
   claim to `-O0`.

20. **A new run's row is marked "pointed at" because it shares an index with the old run's point**
   (reproduced by the reviewer; `row_marks` compares only `frame.index`). Indices restart at 0
   in every run. While writes fail, a new source's frame #2 stays marked as pointed at, though
   the bar describes the old run's #2. Without any failure, a new run that starts within 100 ms
   of a click marks the wrong row until the next write. *Fix:* each run gets a generation,
   carried in the context; a row is marked only for the same run and an equal frame.
21. **The price plot drops a point at even widths, and with it a spike** (reproduced by the
   reviewer). Up to `width / 2` buckets give up to `width` points, but `PlotLines` draws its frame
   one pixel narrower than the space available and then samples. At the UI tests' width, a spike
   at frame 441 of 884 is never drawn, while one at frame 443 is. *Fix:* size the reduction from
   the frame's real width, or draw each bucket as its own vertical segment.
22. **A context file that is never written is reported only inside the window** (reproduced by
   the reviewer). `--exit-after` with an unwritable `--context-file` exits 0, and its output
   doesn't mention the failure. *Fix:* report the first failed write, and a failed `finish()`, on
   stderr; exit nonzero if `finish()` failed.
23. **An empty datagram followed by the sender closing loses the messages after it**
   (reproduced by the reviewer). After `recv` returns 0, `peer_closed` sees the shutdown and
   calls it a clean disconnect while datagrams are still queued: the next frame is lost and
   nothing is reported. *Fix:* peek for another queued datagram first; extend the test to a
   closed sender.
24. **A failed `accept4` is silent** (reproduced by the reviewer with `EMFILE`). Every failure
   counts as "nobody waiting", so the viewer shows "waiting" with no reason while a source sits
   in the queue. *Fix:* only `EAGAIN`, `EINTR` and `ECONNABORTED` mean that; record any other.
25. **`connect_to` with fewer than one attempt reports "Connection refused"** without trying
   (reproduced by the reviewer). *Fix:* refuse it, or make it a precondition.

## B. Losing the user's work
26. **A connection that sends nothing valid wipes the run being examined** (read; `ViewerApp::receive`
   calls `begin_connection()` on any new sender). A connection that sends garbage, or nothing,
   discards the previous, complete run and the point. *Fix:* start the new connection's run at its
   first valid message; test with a garbage-only connection and an empty one.
27. **The checks of the tests overwrite edits made while they run** (read; reproduced by the
   reviewer). `mutants.py` and `selection_check.py` restore the source they captured before
   mutating it, so saving that file during the run loses the edit. They also build in `build/debug`,
   the working build, which the editor's build may be using at the same moment. *Fix:* run both in
   a temporary `git worktree`, never in the working tree.

## C. Test machinery that can answer wrongly
28. **A program with no tests is recorded as passed** (read; reproduced by the reviewer). A
    manifest program with no tests in CTest passes `--check-registration`, and `all_skipped`
    ignores it (`if tests and ...`). For example, every test renamed `DISABLED_` is recorded as
    passed with nothing tested. *Fix:* registration requires at least one test per program; a
    program with none is never recorded.
29. **`mutants.py` ignores a failed restore of the warnings setting** (read). The `finally`
    ignores `set_werror`'s result, so a failed reconfigure leaves `build/debug` without
    `-Werror`, silently. Moving the check to a worktree (item 27) removes this too.
30. **`npm ci` reruns on every full run** (reproduced: "added 5 packages" in the sanitizer build
    with nothing changed). Both builds declare the same output in the source tree, so each sees
    the other's install as newer. Harmless under the lock, but slow and noisy. *Fix:* one stamp
    per build directory, keyed by the lock file.

## D. Input refused late, or not at all
31. **An empty `--socket ""` is accepted** (reproduced). The viewer binds a name Linux chooses
    (autobind), which no source can know; the frame source retries for seconds before failing.
    *Fix:* both parsers refuse it, with a test.
32. **`--exit-after inf` is accepted and never exits** (reproduced), though 0 already means
    "never". *Fix:* refuse a value that isn't finite; test it.
33. **The extension never listens for `onDidTerminateDebugSession`** (read). An adapter running
    as a server, or inline, may end a session without `onExit`, and its entry then stays "running"
    or "stopped" for good.
34. **Self-stop says nothing when `/proc/self/status` can't be read** (read; `tracer_attached_now`).
    A failed read means "no tracer", so a debugger session runs freely with no word why. *Fix:*
    warn once on stderr when self-stop is on and the file can't be read.

## E. Test gaps
35. **Nothing checks the anchor at `-O2`** (read). Every build is `-O0`; removing the anchor's
    `asm volatile` clobber passes every test (reproduced by the reviewer). DEBUGGER-LOCKSTEP.md's
    rule 1 was checked once, by hand. *Fix:* a `check`-tier test that compiles the anchor at `-O2`
    and checks with `nm` and `objdump` that it keeps its own address and its store. The reviewer found that `nm` and `objdump` pass even
    with `noinline` and the clobber removed: the caller is in another file and there is no LTO,
    so nothing can merge or inline it. The rule's measures defend against LTO or a caller in the
    same file. A useful check is a debugger run at `-O2`, which also catches item 19.
36. **Nothing checks that frame N is sent before the stop for frame N** (reproduced by the
    reviewer): with the anchor called before `send_message`, every test passes, since the
    debugger tests count the messages only after the run. That is the lockstep property itself.
    *Fix:* the debugger test's driver counts, at each stop, the messages already waiting.
37. **The debugger tests throw away the stop's line number** (read; their patterns end in
    `:\d+`), which is how item 17 passes. *Fix:* check the line: the `int3` line for self-stop,
    the first statement for a breakpoint by name.
38. **The test scripts have no tests of their own** (read; reproduced by the reviewer).
    `check_registration` returning nothing, `all_skipped` returning nothing, or the `libubsan`
    check removed: all pass CI. `selection_check.py` never checks that a failed run is not
    recorded, or that the slow tier isn't run. *Fix:* a Python `unittest` program in the `unit`
    tier, covering parsing, `all_skipped`, registration and the pass record; and a run case with a
    test made to fail.
39. **Viewer** (mutants that survive, reproduced by the reviewer):
    - nothing checks the source's error through `ViewerApp`; deleting `source_error = receiver.error()` passes;
    - nothing checks `ViewerApp::finish`;
    - nothing checks "seconds since last frame"'s value, only that it appears;
    - nothing checks the plot's points; dropping each bucket's high passes. A pure `plot_points` function would let a unit test check them;
    - nothing checks the envelope's merges in detail: counting the last bucket as full after a merge passes, and so does a `reduced()` that drops its last, partial group, which hides the newest frames;
    - nothing checks that the table draws the followed rows' colour (Dear ImGui doesn't expose it), yet RUNNING.md says the UI tests check the highlights.
40. **The frame source and anchor:**
    - Nothing checks that `--interval-ms` is honoured; removing the sleep passes every test.
    - A `set_self_stop` that does nothing is caught only by the debugger tier.
    - `lldb_self_stop.py`'s failure branches never run.
    - The frame source's failure messages are never checked, only its exit code (item 18 survives).
    - `last_anchored_frame()` before any call ("none yet") is untested.
    - Two unit tests assume no tracer, so they fail when the test program is run under GDB or strace.
41. **The extension:**
    - No test of `onExit(0)` after a program's own exit (item 2), or of `onError`.
    - The concurrent-reader test doesn't assert that its reads overlapped the writes, so it can pass without testing anything on another machine.
    - `extension.test.js` waits a fixed 20 ms for asynchronous work.
    - A stop going stale while members are being read is untested.
    - The reviewer ran 24 more mutants of the extension, and 11 survived:
      - `stop()` not taking a new `seq`;
      - a session in a non-Fillscope folder of a mixed workspace being published;
      - the write-failure catch in `extension.js` removed;
      - the `.catch` on a failed description, and the logged line, never run;
      - a stop without a thread, followed by a single-thread continue;
      - the `text` fallback;
      - the scope chosen by name, since every fixture lists Locals first.

      So DEBUGGER-LOCKSTEP.md's "each shown, by a mutant, to fail a test when broken" is not yet true.
    - The fake `vscode` calls `onExit(1, undefined)`, which CodeLLDB never causes.
    - No test covers restart, child sessions or `terminated` through `extension.js`.
42. **Mutants name whole programs:** 22 of 60 name `viewer_ui_tests` or
    `vscode_debug_state_tests`, so nothing makes sure the intended test is the one that fails. The
    reviewer checked all 22, and each was caught by its intended test. *Fix:* name single tests,
    through Node's `--test-name-pattern` and the Test Engine's filter.

43. **More viewer and channel gaps** (each reproduced by the reviewer with a mutant that passes):
    - a retry when the context has returned to what was published (publish A, fail on B, back to A): removing `&& !error_` passes, and the test that says "retried, though unchanged" changes the context;
    - the pointed-at row as drawn: passing `false` to its `Selectable` passes, and two UI tests whose names claim the table's marks check only `row_marks`;
    - the `kMaxMessagesPerDrain` bound: removing it passes;
    - `Channel.ConnectDoesNotBlockWhenTheListenersQueueIsFull` hangs on failure instead of failing, since `std::async`'s destructor waits for the blocked connect;
    - one check in `a_new_run_drops_the_point_and_keeps_the_follow` can't fail: the follow's `seq` is higher only because it was clicked second.

## F. Code quality
44. **Viewer:**
    - `ViewerApp::receive` allocates a new vector every paint;
    - `bar_text` formats its strings every paint, though the published context rarely changes;
    - the plot allocates two vectors every paint;
    - `PriceEnvelope::append`'s even-count branch can never run: a merge happens at 4,097 buckets, always odd (assert `kMaxBuckets` is even, and keep one path);
    - `net.h` says later senders wait in "one place"; Linux queues the backlog plus one;
    - `parse_number`'s constraint lets `bool` through, which then fails deep in the template, the error the constraint's comment says it prevents;
    - `main.cpp` ignores the results of the Dear ImGui backends' `Init` calls;
    - `test::ChildProcess::started()` turns false after `wait()`;
    - `Run` keeps every frame, so memory grows with the run (about 32 bytes a frame, so 10 million frames is 320 MB). State the bound, or cap the run.
45. **Anchor and frame source:**
    - with self-stop on, the anchor reads `/proc/self/status` on every call, through `ifstream` and `ostringstream`, which Step Into walks through;
    - `last_sent` in `run_frame_source` duplicates `options.frames > 0`;
    - the `noinline` comment overstates: the function is emitted anyway (it has external linkage), debuggers bind to inlined copies too, and without LTO the call can't be inlined at all;
    - `draw_uniform` has an unstated precondition, `low <= high`;
    - `(option == "--frames" ? options.frames : options.seed) = *number` sends any other numeric option added later to `seed`;
    - the loop sleeps after the last frame too, so a run ends one interval late;
    - `main` builds `std::span{argv + 1, argv + argc}`, an invalid range if `argc` is 0 (Linux 5.18 and later always pass `argv[0]`).
46. **Extension:**
    - `extension.js` duplicates `messageOf`;
    - `sessionEnded` builds a fake session to call `change`;
    - pending resumes are keyed `"id#seq"`, so ending a session scans every key;
    - two workspace folders reaching one directory through a symlink get two publishers writing one file (key by `realpath`);
    - `|| got.length > FRAME_LIMIT` in `countFrames` can never decide the result;
    - several defensive steps no test can observe are commented as if they were load-bearing: the `StaleStop` rethrow, deleting pending resumes at a session's end, `hasEnded` in `onClientMessage`;
    - `extension.js`'s header has one merged line of more than 170 characters;
    - after `stop()`, the output line names one session of the several that changed.
47. **Test machinery:**
    - `iterate.py` lists CTest's tests twice, with the same parsing code;
    - `fingerprints()` reuses the name `shared` for a hash object and then for a string;
    - `fillscope_gtest` re-parses two options only to pass them on.

## G. Documents
48. **ARCHITECTURE.md:**
    - says the `static_assert`s check that the payloads are trivially copyable; they check `has_unique_object_representations`, which also rules out padding;
    - "no spike is lost" (items 5 and 21), and "reduced to its own width" (half the width, drawn one pixel narrower);
    - "the viewer publishes the moment anything changes" contradicts §6, where changes the user didn't make are written at most once per 100 ms;
    - "SourceHello opens every connection" overclaims, also in `message.h`: the viewer accepts connections without one, and tests rely on it.
49. **DEBUGGER-LOCKSTEP.md:**
    - cites `Anchor.SelfStopWithoutADebuggerDoesNotStop`; the test is `Anchor.SelfStopWithoutATracerDoesNotStop`;
    - rule 1's "Checked in the `-O2` build" reads as a standing check, and the check passes without the measures (item 35);
    - "a breakpoint by name lands on the first statement": LLDB places it at the `if`, after both stores (GDB at the first statement);
    - "Step Over from the anchor returns to the producer's loop" holds from the self-stop only; from a breakpoint by name it steps through the anchor first;
    - "`frame_index` readable" holds at `-O0` only (item 19);
    - "expensive scopes are never read" (item 6);
    - an adapter crash's `endReason`, which can't happen with CodeLLDB (item 2);
    - "nothing changes a session after it has ended": a restart under the same id does, by design;
    - "'exited' when the program ends": also on `terminated` (item 8);
    - the writer "still running" is true only if VS Code deactivates the extension; the process id is the reliable check;
    - "writes only for a session in a ... folder that contains its own source" omits that a session with no folder goes to the first Fillscope folder;
    - the resuming requests listed include `goto` and `restartFrame` (item 12).
50. **RUNNING.md:**
    - the example output line says `anchor.cpp:69`, the function's signature, where no stop lands (also in `debug_state.js`'s comment);
    - "F10 from the anchor steps back into the loop": from the self-stop only;
    - "Each Continue adds one point": each frame adds two plotted points (a bucket's low and high), and after 4,096 frames, buckets merge;
    - "Every form builds first" isn't true of the forms for scripts;
    - "the table's highlights" are tested only through `row_marks` (item 39);
    - "Both always restore the source, even when stopped by a signal" isn't true of SIGKILL, or of an edit made during the run (item 27).
51. **AI-USE.md:**
    - the extension's row says "four mutants" (there are 17);
    - the selection row doesn't say `selection_check.py` now also runs and records.
52. **`context.h`** says `running: true` with a dead process "means it crashed"; a SIGINT leaves
    exactly that (item 1).
53. **`extension.js`** says `onExit` is "the only word that the session is over" after a crash; it
    also fires after every normal end.
54. **For the owner:**
    - the charter's decision 3 says the app "logs the context of what is selected"; it writes a file, replaced on every change, as README.md now says;
    - README.md lacks the Purpose / Status / Read when lines that CLAUDE.md requires of every document.

## H. From the test machinery reviewer's fuller report
Added after the backlog was first written; each confirmed by the reviewer with a probe in a copy
of the repository, and checked by reading the code at the place named.
59. **`mutants.py` turns `-Werror` off when the cache holds a true value other than "ON"**:
    `FILLSCOPE_WERROR=1` is stored as `BOOL=1`, read as off, and restored as off.
60. **The "written in place" mutant is caught only by a timing race** (missed 2 of 24 under
    load), so a loaded CI runner can report it falsely. *Fix:* a deterministic check that the
    write goes to a temporary file that is then renamed.
61. **Nothing stops two runs using `build/debug` at once** (`iterate.py`, `run-tests.sh`,
    `mutants.py`, `selection_check.py`), and the pass record's update is unlocked. *Fix:* each takes
    a lock on the build directory.
62. **`iterate.py` fingerprints the `node` on `PATH`, not the one CMake found for the tests**, and
    counts it for every program. *Fix:* the tests' `node` as a run-time file of the two Node
    programs only.
63. **`--check-registration` accepts a test that copies a program's labels**; its command is
    never fingerprinted. *Fix:* compare each test's command with its program.
64. **Smaller:**
    - `mutants.py` crashes on an unconfigured build directory;
    - the documents' glob walks `node_modules`, so each `npm ci` re-runs CMake;
    - "no fast test program affected" can never print, since `docs_tests` always runs;
    - `cancel-in-progress` cancels runs on `main` too;
    - `hashFiles` adds nothing to the CI cache key;
    - `FILLSCOPE_WERROR` counts in every fingerprint though it matters only at configure time;
    - `selection_check.py` has no case for `tools/frame_source/main.cpp`, the extension's `package.json` or `tsconfig.json`, `net.cpp`, or a deleted or renamed file;
    - mutants are missing for `viewer_options.cpp`, the frame source's `main.cpp`, `lldb_self_stop.py`, and the `ui`, `check` and `debugger` tiers' timeouts;
    - the Node tests' 2 s limit has about 1.8 times headroom in CI's sanitizer run;
    - comments in `tests/CMakeLists.txt` and the extension's `CMakeLists.txt` are stale, and RUNNING.md's "the Node.js that runs it" is not what is fingerprinted.

## Left in place deliberately
65. **The debug-state file shows one stop per session: the latest.** In all-stop debugging, which
    GDB and LLDB use here, that is the whole truth. In non-stop debugging, another thread can still
    be stopped after the published one resumes. *Fix, if non-stop debugging is ever used:* publish
    stops per thread.
66. **Two VS Code windows on one Fillscope folder overwrite each other's debug-state file.** The
    file names its writer's process id, and the documents say to use one window.
67. **A writer that crashes leaves its file saying "running"** (a signal it can handle is item 1).
    Both files name their writer's process id and start time; a reader that must be sure checks
    that the process is alive.
68. **CI's Fedora packages aren't pinned:** the repositories keep only the newest update of each,
    so each run records the toolchain's versions instead.
