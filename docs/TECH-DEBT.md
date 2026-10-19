# Tech debt backlog

**Purpose:** known defects, test gaps and other debt, ranked, each with its evidence and a fix.
**Status:** live plan; an item is removed when fixed (git keeps it), and the file is deleted when
empty. **Read when:** choosing what to fix, or before changing code an item names.

From the first sweep, 2026-10-19: four independent reviews (the channel, anchor and frame source;
the viewer; the test and build machinery; the debug-state extension), plus a pass over the
documents. Every item was verified. **Reproduced** means it was shown happening, with a probe or
the real program. **Read** means it was confirmed in the code at the place named. Items are grouped
by what goes wrong; within a group, the most harmful come first.

## A. Published state that can be false
The charter's first priority is that nothing displayed or published is untrue. These break it.

1. **Frames from two sources can merge into one run, under the wrong seed** (reproduced).
   `ViewerState::apply` appends a frame with no `SourceHello` to whatever run exists. So a source
   that doesn't describe itself, connecting after one that did, has its frames added to the old
   run. They inherit the old seed: a false replay address in the bar and the file. Their indices
   repeat, so `point_label` IDs collide in Dear ImGui and the pointed-row highlight lights several
   rows. This contradicts `viewer_state.h` ("frames shown always belong to one run") and
   ARCHITECTURE.md §6. *Fix:* the receiver reports each new connection, and a new connection
   always starts a new run.
2. **A new run makes the file say the user stopped pointing** (reproduced). `Selection::new_run()`
   calls `clear_point()`, which takes a new sequence number, so the point's seq becomes the higher
   one: "what the user did last", per `context.h`, the JSON's `about` text and ARCHITECTURE.md.
   The user did nothing. *Fix:* the viewer's own clear doesn't take a user seq, or the text says
   "changed last".
3. **The debug-state extension can publish an out-of-date stop after a restart** (reproduced).
   `sessionStarted` deletes an ended session's generation counter. A session restarted with the
   same id (VS Code's emulated restart) starts counting again, and a stop still being read from
   before the restart passes the staleness check. *Fix:* one counter that only increases.
4. **An event after a session ends brings it back to "running", for good** (reproduced). Nothing
   checks for an ended session before changing it. *Fix:* ignore events for an ended session.
5. **"Running" is published when a resuming request is sent, not when it succeeds** (read). If the
   debugger rejects it (e.g. `stepBack` without reverse debugging), the program is still stopped
   but the file says running and the stop's details are gone. *Fix:* publish on the request's
   successful response, or restore on a failed one.
6. **Neither published file says whether its writer is still alive** (read).
   - `.fillscope/context.json` keeps "connected: true" and the last point after the viewer exits.
   - `.fillscope/debug-state.json` keeps old sessions, often "stopped", after VS Code reloads.
   - Both restart `seq` at 1 in each process, so a reader comparing sequence numbers sees them go
     backwards, while the comments say `seq` "increases with every write".
   - If the viewer's first write fails, the previous run's file stays and is read as current.
   - Two VS Code windows on one workspace overwrite each other's sessions.

   *Fix:* a writer id (process id and start time) in each file, an "ended" write at exit, a fresh
   file at start, and comments that say "within one writer".
7. **The bar can be right while the table and tooltips are not** (read). After a failed write, the
   table still highlights the live selection, which the bar and file don't have; the tooltips say
   a click "sends it to the assistant now" while writes are failing. *Fix:* draw the highlights from
   the published context, and word the tooltips conditionally.
8. **`frameCount` treats the debugger's `totalFrames` as exact** (read). The Debug Adapter Protocol
   treats it as a hint that may overstate, so a published "true count" can be wrong. *Fix:* trust
   it only when the reply was shorter than requested; otherwise publish null.
9. **"The same seed gives the same frames" holds for one standard library only** (read).
   `std::mt19937_64` is fully specified, but `std::uniform_int_distribution` is not: libstdc++ and
   libc++ differ, and libstdc++ has changed its algorithm before. Replay addresses would silently
   name different frames under another toolchain. *Fix:* draw bounded values in project code, or
   state the limit everywhere the claim is made (`message.h`, `frame_source.h`, ARCHITECTURE.md).
10. **Self-stop fires under any tracer, not just a debugger** (reproduced by the reviewer; read).
    `debugger_attached()` checks for a nonzero `TracerPid`, which `strace` or `rr` also set; under
    `strace`, `--self-stop` kills the frame source with SIGTRAP. *Fix:* say "any tracer" in the
    comments and docs, or make self-stop opt-in per debugger.
11. **The LLDB self-stop script labels any signal "SIGTRAP"** (read). A crash (SIGSEGV, SIGABRT)
    would be reported, and accepted by the test, as a self-stop. *Fix:* read the signal number.

## B. Test machinery that can give a wrong answer
12. **`iterate.py` records the files as they are after the run, not as tested** (read). The
    fingerprint is computed after `ctest` returns, so a document, script or `.js` file edited
    during a run is recorded as passed untested and then never re-run. *Fix:* fingerprint before
    running.
13. **`run-tests.sh` has the same problem over minutes** (read). It records `build/debug` as passed
    after the whole sanitizer build and run; a rebuild or edit in that time is recorded untested.
14. **`docs_tests` doesn't fingerprint its link targets** (read). Deleting or renaming a linked
    file (`LICENSE`, `src/`, `ci.yml`) leaves the test "unchanged", so `iterate.py` doesn't re-run
    it; only a full run catches the broken link. This contradicts `iterate.py`'s own comment.
15. **The mutation check has no baseline** (read). A mutant counts as caught if its tests fail for
    any reason, so a test already failing or flaky makes every mutant naming it "caught"
    (`viewer_ui_tests` is named by ten). *Fix:* run each regex on the real code first; it must pass.
16. **The LLDB test's `PYTHONPATH` has no effect** (reproduced). `python3 -I` ignores `PYTHON*`
    variables; the test works only because Fedora installs LLDB's module in the default path, and
    `lldb_self_stop.py`'s usage line is wrong. *Fix:* insert `lldb -P` into `sys.path` in the script.
17. **`lockstep_debugger_tests` is recorded as passed when every test was skipped** (read).
    Locally, without gdb or lldb, the slow-tier reminder then disappears.
18. **Nothing checks that every CTest test is registered through `fillscope_test_program`** (read).
    One that isn't would be invisible to `iterate.py` and skipped by the sanitizer-linkage check.
19. **`FILLSCOPE_FRAME_SOURCE` is compiled into every test program**, but only programs that list
    the frame source in `RUNTIME_DEPS` are re-run when it changes (read). A new test that starts it
    without listing it would miss re-runs. *Fix:* attach the define with the dependency.
20. **The extension's fingerprint and type check cover different files** (read): the fingerprint
    globs `src/*.js` and `*.test.js`; the type check includes subdirectories and any `.js`.
21. **The sanitizer check looks for AddressSanitizer only** (read): a build that lost UBSan passes,
    and nothing checks that `build/debug` isn't sanitized.
22. **`mutants.py` and `selection_check.py` restore sources only on Ctrl-C** (read): SIGTERM or a
    closed terminal leaves a mutated file; the final rebuild's result is ignored.
23. **Smaller robustness gaps** (read):
    - `last-pass.json` is read-modified-written non-atomically; a torn write makes every later
      `iterate.py` run crash.
    - `build_files()` excludes only a directory named exactly `build`, not `build-*/` or `out/`.
    - Unknown arguments to `iterate.py` go straight to ctest rather than a usage message;
      `os.cpu_count()` ignores CPU affinity where `nproc` doesn't.
    - The node, environment and system libraries aren't fingerprinted (full runs cover them).
    - Paths with spaces break the shell commands in the debugger tests and the whitespace-split
      manifest.
    - `npm ci` deletes the shared `node_modules` while the other build's type check may be reading
      it.

## C. Crashes and unhandled input
24. **The frame source aborts on a bad number and accepts others it shouldn't** (reproduced).
    `std::stoull`/`std::stoi` throw uncaught on `--frames abc`; `--frames 5x` is read as 5,
    `--frames -1` as 18446744073709551615, `--seed 0x10` as 0. The header says malformed input
    returns nothing. *Fix:* `std::from_chars` requiring the whole argument, returning
    `std::expected` with the reason. The viewer's `--exit-after` (`std::stod`) has the same flaw.
25. **A bare filename for `--context-file` makes every write fail** (reproduced):
    `create_directories("")` fails with "Invalid argument". *Fix:* skip it when there's no parent.
26. **An empty datagram is taken as a disconnect** (reproduced by the reviewer; read). `recv`
    returns 0 for both; the sender is dropped with no error and its later messages are lost.
27. **The receiver's error is never shown, and goes stale** (reproduced by the reviewer; read).
    `MessageReceiver::error()` is read nowhere, so a garbage sender just looks like "waiting"; and
    it isn't cleared when a good sender connects.
28. **A third concurrent sender blocks in `connect` forever** (read): blocking sockets with a
    backlog of 1. `connect_to` also sleeps after its final failed attempt.
29. **No `SOCK_CLOEXEC`** on `socket()`/`accept4()` (read): descriptors leak into child processes
    (the tests' gdb and lldb, anything the viewer starts).
30. **Socket names longer than 107 bytes are silently truncated** (reproduced by the reviewer), so
    distinct long names collide.
31. **`encode` can overrun its buffer if a third message kind is added** (read):
    `kMaxMessageSize` names the two types by hand, and nothing asserts a payload fits.
32. **`ChildProcess::wait` reports success when `waitpid` fails** (read): `status` starts at 0 and
    the return value is unchecked. `run_command` ignores `pclose`'s status.
33. **`last_anchored_frame()` returns 0 both for "frame 0" and "none"** (reproduced): `--frames 0`
    prints "last anchored frame 0". *Fix:* `std::optional`.
34. **The viewer's `main` gives no message when GLFW fails,** and doesn't call `glfwTerminate`
    when window creation fails (read).
35. **The anchor's `int3` has no architecture guard** (read): the build fails on aarch64. Record
    why `std::breakpoint` (C++26, out of line in libstdc++) isn't used instead.
36. **`TracerPid:` is searched for anywhere in the status file** (read): a thread named
    "TracerPid:…" would read as traced. *Fix:* match at the start of a line.

## D. The debug-state extension and the Debug Adapter Protocol
37. **Threads are ignored** (read). A single-thread step, or `allThreadsContinued: false`, marks
    the whole session running; a stop on another thread replaces the stack. Fine for all-stop gdb
    and lldb as used today; wrong in non-stop mode.
38. **A stop with no thread id is published with no stack** (read); the protocol allows it when
    all threads stopped, and VS Code then asks for `threads`.
39. **`restart`, `terminated`, `exited` and an adapter crash aren't handled** (read): a restarted
    program keeps the old stop; an exit shows only at session end; a crashed adapter's entry stays
    forever.
40. **Multi-root workspaces:** all sessions go to the first Fillscope folder's file, and folders
    added later are never checked (read).
41. **Child sessions** get entries with no link to their parent, which stays "running" (read).
42. **A failed write leaves the temporary file** (read).
43. **Up to 101 requests per stop, and they continue after the stop is stale** (read); the scope
    fallback can pick an expensive one (Registers). These queue ahead of VS Code's own requests.

## E. Test gaps
44. **Viewer:** no test of a new run while pointing or following; a real source reconnecting; a
    source without a hello after one with it (item 1); the bar lagging a held-back change; failure
    then recovery; the `bar_text` branches for an undescribed source, "no replay address",
    "(first N listed)" and "1 frame"; `latestFrameIndex` other than null. The test that compares
    the file with `published()` takes `writtenAtMs` from the file itself.
45. **Channel and frame source:** no test of an oversized datagram (removing the `+ 1` in the
    receive buffer passes every test); of the wire layout itself (a consistent change to both
    `encode` and `decode` passes); of malformed numeric arguments; of the "viewer went away" path;
    of `--frames 0`; of a new sender after a bad one. One test joins a thread with no timeout.
46. **Extension:** no test of overlapping stops, a session ending mid-stop, a restart, events
    after the end, a failed resuming request, multiple threads, a stop with no thread, an empty
    stack, a failed `scopes`, `childrenError`, ignored responses, or 4 of the 8 resuming requests.
    `extension.js` is untested; its one test matches source text with a regex. The atomic-write
    test checks neither a failed rename nor a concurrent reader.
47. **The checks of the tests:** `selection_check.py` checks only `--list`, never what is run or
    recorded, and has no case for a run-time file outside the build.

## F. Code to bring to the exemplary standard
The plan's "exemplary code throughout" step works through these.
48. **The viewer's loop is written twice:** in `main.cpp` and in the UI test, and the copy already
    differs (no timing, no `paints`). The test's header and ARCHITECTURE.md §10 overstate it.
    *Fix:* one `step()` in the core, called by both.
49. **Per-paint cost grows with the run:** with an id followed, `make_context` scans every frame
    every paint; `PlotLines` with automatic scale scans every price. The comment that a million
    frames cost "no more per paint than fifty" is true of the table only. `PlotLines` also shows
    one sample per pixel, so spikes vanish on long runs, and prices become floats.
50. **`std::expected` where errors are now lost:** `listen_on` and `send_message` collapse every
    failure to "name taken" or "viewer gone"; the "can't write" error carries no reason.
51. **Smaller C++ items:**
    - Missing includes: `viewer_view.cpp` (`<string_view>`, `<span>`, `<string>`),
      `viewer_test.cpp` (`<format>`, `<unistd.h>`).
    - `ContextPublisher::update(…, bool user_action, …)` is a boolean trap.
    - `send_message` takes `int` rather than `const Fd&`.
    - `std::fprintf` with `%llu` casts, where `std::println` is type-safe.
    - `run_frame_source` lacks `[[nodiscard]]`.
    - `REQUIRE_DEBUGGER` expands to a bare `if`.
    - `setenv("ASAN_OPTIONS", …)` overwrites rather than adds.
    - `run_command` uses a C array.
    - `frame.h`'s check should be `std::has_unique_object_representations_v`, and `SourceHello`
      lacks a size assert.
    - The anchor's globals are commented as "only the calling thread touches these", a
      precondition nothing enforces.
    - `[[gnu::noipa]]` states the anchor's intent more exactly than `noinline`.
    - Row labels and tooltip strings are formatted every paint for every visible row.
52. **The extension's types:** `SessionState` is an intersection where a union on `state` would
    let the checker tell stopped from running; message parameters are `any` though the protocol's
    types are available.

## G. Documents
53. **README.md:** "Nothing is built yet" is stale; "How it's built" states in the present tense
    things that don't exist (ImPlot, the replay command-line tool, recorded demo videos); the
    context is said to go "to a log", not a file replaced on every change.
54. **ARCHITECTURE.md:** "if the context equals what was last published, nothing is written" is
    false after a failed write (it retries); "within 100 ms" is 100 ms plus a paint, and only if
    the write succeeds; it says only a `SourceHello` starts a run (a first frame does too); and
    items 1, 2, 9 and 48.
55. **RUNNING.md:**
    - The `unit` tier's "2 s per test" is per program for the Node tests.
    - `--list` also builds first.
    - "Every run prints its five slowest" isn't true when a run fails.
    - A stray paragraph splits the list of test programs.
    - The stated needs omit python3, gdb, lldb and python3-lldb.
    - `ctest -R viewer` finds the UI tests but none of the 22 unit tests, which carry their
      GoogleTest names; `-L exe:viewer_tests` is the way, and isn't mentioned.
56. **DEBUGGER-LOCKSTEP.md:** it says each extension requirement is "checked by its tests"; the
    atomic replacement, staleness after a session ends, and activation aren't.
57. **Comments:**
    - The receiver's comments overstate it: "never blocking", but `drain` has no bound;
      "the next one accepted", but only on a later call.
    - "8-byte aligned" payload: the buffers are byte-aligned, and only `memcpy` makes it harmless.
    - `viewer_state.h` says it holds what was received, but it also holds the viewer's own timing.
    - The extension's generation comment omits the stop's own increment and the reset.
    - `tests/CMakeLists.txt` omits the any-`CMakeLists.txt` rule.

## H. CI and build hygiene
58. **Warnings don't fail anything:** `-Wall -Wextra` with no `-Werror` in CI.
59. **The workflow has no `permissions:` or `timeout-minutes`;** push plus pull_request runs
    pull-request branches twice; the Fedora image and packages aren't pinned; fetched dependencies
    use tags without `GIT_SHALLOW`, and nothing is cached.
