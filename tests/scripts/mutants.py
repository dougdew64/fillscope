# Checks that the tests catch bugs: each mutant breaks the code in one deliberate way, and the
# tests named for it must then fail, though they pass on the real code (checked first). The source
# is always restored, even on Ctrl-C, SIGTERM or a closed terminal.
#
#   python3 -I tests/scripts/mutants.py [BUILD_DIR]     (default build/debug)
#
# A mutant whose text no longer matches the source is an error, not a pass: the check must not go
# silently stale when the code changes.
import pathlib
import signal
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

ANCHOR = "src/lockstep/anchor.cpp"
CHANNEL = "src/channel/net.cpp"
SOURCE = "tools/frame_source/frame_source.cpp"
VIEW = "src/viewer/viewer_view.cpp"
MESSAGE = "src/channel/message.cpp"
PARSE = "src/common/parse.h"
SOURCE_HEADER = "tools/frame_source/frame_source.h"
CONTEXT = "src/viewer/context.cpp"
PUBLISHER = "src/viewer/context_publisher.cpp"
VIEWER_STATE = "src/viewer/viewer_state.cpp"
VIEWER_STATE_HEADER = "src/viewer/viewer_state.h"
DOC_LINKS = "tests/docs/doc_links_test.cpp"
DEBUG_STATE = "tools/vscode-debug-state/src/debug_state.js"
EXTENSION = "tools/vscode-debug-state/src/extension.js"

# (name, file, [(old, new), ...], CTest regex of tests that must fail)
MUTANTS = [
    ("indices skip", SOURCE,
     [(".index = next_index_++,", ".index = next_index_ += 2,")],
     "FrameGenerator.IndicesAreConsecutive"),
    ("price step too large", SOURCE,
     [("draw_uniform(engine_, 0, 2 * kMaxPriceStep)", "draw_uniform(engine_, 0, 2 * kMaxPriceStep + 1)")],
     "FrameGenerator.PriceWalks|FrameGenerator.SeedFortyTwo"),
    ("the bounded draw keeps the biased draws", SOURCE_HEADER,
     [("while (raw < rejected_below) {", "while (false) {")],
     "DrawUniform.RejectsTheDraws"),
    ("a number is read from only part of the text", PARSE,
     [("if (error != std::errc{} || stopped_at != last) {", "if (error != std::errc{}) {")],
     "ParseSourceArgs.RejectsNumbers"),
    ("any TracerPid line means a tracer", ANCHOR,
     [("return pid.has_value() && *pid != 0;", "return pid.has_value();")],
     "TracerAttached.ReadsTracerPid"),
    ("anchor doesn't record", ANCHOR,
     [("fillscope::g_last_anchor_index = frame_index;", "fillscope::g_last_anchor_index = 0;")],
     "Anchor.RecordsEachFrameIndex"),
    ("self-stop ignores the tracer check", ANCHOR,
     [("if (fillscope::g_self_stop && fillscope::tracer_attached_now())", "if (fillscope::g_self_stop)")],
     "Anchor.SelfStopWithoutATracer"),
    # The bug found in VS Code, as it was: the anchor's first statement an std::atomic store,
    # inlined from libstdc++ even at -O0. LLDB's breakpoint by name lands in atomic_base.h; GDB's
    # stays in our file, so only the LLDB test can catch it.
    ("anchor's first statement inlined from a header", ANCHOR,
     [("#include <algorithm>", "#include <algorithm>\n#include <atomic>"),
      ("std::uint64_t g_last_anchor_index = 0;", "std::atomic<std::uint64_t> g_last_anchor_index{0};"),
      ("fillscope::g_last_anchor_index = frame_index;",
       "fillscope::g_last_anchor_index.store(frame_index, std::memory_order_release);")],
     "LockstepDebugger.LldbBreakpointByName"),
    # A unit test that becomes slow must fail as slow: the unit tier's timeout is 2 s.
    ("a unit test takes 3 seconds", ANCHOR,
     [("#include <algorithm>", "#include <algorithm>\n#include <thread>"),
      ('    constexpr std::string_view key = "TracerPid:";',
       '    std::this_thread::sleep_for(std::chrono::seconds{3});\n    constexpr std::string_view key = "TracerPid:";')],
     "TracerAttached.ReadsTracerPid"),
    ("frame source skips the anchor", SOURCE,
     [("        fillscope_frame_anchor(frame.index);\n", "")],
     "LockstepDebugger"),
    ("--socket ignored", SOURCE,
     [("options.socket_name = value;", "(void)value;")],
     "ParseSourceArgs|LockstepDebugger|viewer_ui_tests"),
    ("messages reordered on receipt", CHANNEL,
     [("out.push_back(*message);", "out.insert(out.begin(), *message);")],
     "Channel.EveryMessageSentArrivesInOrder|RunFrameSource.DescribesTheRun|viewer_ui_tests"),
    ("a datagram of the wrong size decodes", MESSAGE,
     [("if (datagram.size() != expected_size) {", "if (false) {")],
     "Message.DecodeRejects"),
    ("an oversized datagram is cut to fit", CHANNEL,
     [("std::array<std::byte, kMaxMessageSize + 1> buffer;", "std::array<std::byte, kMaxMessageSize> buffer;")],
     "Channel.AnOversizedDatagram"),
    ("a disconnect is taken for an empty message", CHANNEL,
     [("if (peer_closed(sender_)) {", "if (false) {")],
     "Channel.ReceiverReportsConnectAndDisconnect"),
    ("an empty message is taken for a disconnect", CHANNEL,
     [('drop_sender("an empty message");', "sender_.reset();")],
     "Channel.AnEmptyDatagram"),
    ("a new sender inherits the last one's error", CHANNEL,
     [("            error_.reset();\n", "")],
     "Channel.ANewSenderClears"),
    ("a full listener queue blocks connect", CHANNEL,
     [("momentarily full.\n        Fd socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};",
       "momentarily full.\n        Fd socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};")],
     "Channel.ConnectDoesNotBlock"),
    ("a long socket name is cut instead of refused", CHANNEL,
     [("    if (name.size() > kMaxSocketNameLength) {\n", "    if (false) {\n")],
     "Channel.ANameTooLong"),
    ("a gone receiver isn't recognised", CHANNEL,
     [("return error == std::errc::broken_pipe || error == std::errc::connection_reset;", "return false;")],
     "Channel.SendSaysWhenTheReceiverHasGone"),
    ("frame count off by one", VIEW,
     [('text(std::format("frames received: {}", received));', 'text(std::format("frames received: {}", received + 1));')],
     "viewer_ui_tests"),
    ("always shows connected", VIEW,
     [("    if (state.source_connected) {\n", "    if (true) {\n")],
     "viewer_ui_tests"),
    ("last frame's index off by one", VIEW,
     [("price {}  qty {}\", last.index, last.id", "price {}  qty {}\", last.index + 1, last.id")],
     "viewer_ui_tests"),
    ("plot and table hidden until many frames", VIEW,
     [("if (view.state.run && !view.state.run->frames().empty()) {",
       "if (view.state.run && view.state.run->frames().size() > 5) {")],
     "viewer_ui_tests"),
    ("clicking an id also points at its frame", VIEW,
     [("                actions.follow = frame.id;\n",
       "                actions.follow = frame.id;\n                actions.point_at = frame;\n")],
     "viewer_ui_tests"),
    # The table's marks, like the bar, must say what was published, not what the user clicked.
    ("the table marks the live selection", VIEW,
     [(".pointed = published->point && published->point->frame.index == frame.index,",
       ".pointed = true,")],
     "viewer_ui_tests"),
    ("a hello counts as a frame for the timing", "src/viewer/viewer_app.cpp",
     [("    if (std::holds_alternative<Frame>(message)) {\n", "    if (true) {\n")],
     "viewer_ui_tests"),
    ("a new source joins the last one's run", "src/viewer/viewer_app.cpp",
     [("    if (receiver.drain(received).new_sender) {\n        begin_connection();\n    }\n",
       "    (void)receiver.drain(received);\n")],
     "viewer_ui_tests"),
    ("a new connection keeps the old run", VIEWER_STATE_HEADER,
     [("void begin_connection() { run.reset(); }", "void begin_connection() {}")],
     "ViewerState.ANewConnectionNeverInherits"),
    ("an id's frames are indexed under the wrong id", VIEWER_STATE,
     [("indices_by_id_[frame.id].push_back(frame.index);", "indices_by_id_[frame.id + 1].push_back(frame.index);")],
     "Run.IndexesEachIdsFrames|MakeContext.FollowLists|viewer_ui_tests"),
    ("the envelope grows without bound", VIEWER_STATE,
     [("    if (buckets_.size() <= kMaxBuckets) {\n", "    if (true) {\n")],
     "PriceEnvelope.StaysBounded"),
    ("merging loses the low", VIEWER_STATE,
     [("return {.low = std::min(a.low, b.low)", "return {.low = a.low")],
     "PriceEnvelope.StaysBounded"),
    ("a new run keeps the old point", CONTEXT,
     [("    pointed_.reset();  // point_seq_ unchanged: the user didn't do this\n", "")],
     "Selection.ANewRunClearsThePoint|viewer_ui_tests"),
    ("a new run takes a sequence number", CONTEXT,
     [("    pointed_.reset();  // point_seq_ unchanged: the user didn't do this\n",
       "    clear_point();\n")],
     "Selection.ANewRunTakesNoSequenceNumber|viewer_ui_tests"),
    ("the replay address names the wrong frame", CONTEXT,
     [(".frame_index = frame->index", ".frame_index = 0")],
     "MakeContext.ThePointHasAReplayAddress|viewer_ui_tests"),
    ("a user action waits like a background change", PUBLISHER,
     [("if (trigger == PublishTrigger::background && last_attempt_ && now - *last_attempt_ < min_interval_) {",
       "if (last_attempt_ && now - *last_attempt_ < min_interval_) {")],
     "Publisher.AUserActionIsWrittenAtOnce"),
    ("an unchanged context is rewritten", PUBLISHER,
     [("    if (published_ == context && !error_) {\n", "    if (false) {\n")],
     "Publisher.AnUnchangedContextIsNotRewritten"),
    # The bar draws what was published; claiming a failed write as published would make it lie.
    ("a failed write is claimed as published", PUBLISHER,
     [("        error_ = std::move(written.error());  // the bar shows this; published_ still says what the file holds\n"
       "        return;\n",
       "        error_ = std::move(written.error());\n        published_ = context;\n        return;\n")],
     "Publisher.AFailedWriteKeeps|viewer_ui_tests"),
    ("a bare file name is refused", PUBLISHER,
     [("if (const std::filesystem::path directory = file.parent_path(); !directory.empty()) {",
       "if (const std::filesystem::path directory = file.parent_path(); true) {")],
     "Publisher.ABareFileName"),
    ("a failed write leaves its temporary file", PUBLISHER,
     [("        ::unlink(temporary.c_str());\n", "")],
     "Publisher.AFailedWriteIsRetried"),
    ("finish says the viewer is still running", PUBLISHER,
     [("writer_.running = false;", "writer_.running = true;")],
     "Publisher.FinishSays"),
    ("a document links to a file that doesn't exist", "README.md",
     [("(docs/RUNNING.md)", "(docs/RUNNING-missing.md)")],
     "DocLinks.EveryRelativeLinkResolves"),
    ("links inside code blocks are followed", DOC_LINKS,
     [("        if (in_code_block) {\n            continue;\n        }\n", "")],
     "DocLinks.FindsRelativeLinksOnly"),
    ("a stale stop is published", DEBUG_STATE,
     [("            if (this.generations.get(session.id) !== generation) {\n"
       "                return; // the program resumed while the details were being read: this stop is stale\n"
       "            }\n", "")],
     "vscode_debug_state_tests"),
    ("only 'continue' resumes", DEBUG_STATE,
     [("if (RESUMING_REQUESTS.has(request.command)) {", "if (request.command === 'continue') {")],
     "vscode_debug_state_tests"),
    ("a refused resume is published as running", DEBUG_STATE,
     [("        if (!response.success) {\n", "        if (false) {\n")],
     "vscode_debug_state_tests"),
    ("a late resume hides a newer stop", DEBUG_STATE,
     [("        if (this.generations.get(session.id) !== pending.generation) {\n", "        if (false) {\n")],
     "vscode_debug_state_tests"),
    # A counter per session restarts with the session, so a stop from before a restart can match.
    ("generations restart with the session", DEBUG_STATE,
     [("        this.generation += 1;\n        this.generations.set(session.id, this.generation);\n",
       "        this.generation = (this.generations.get(session.id) ?? 0) + 1;\n"
       "        this.generations.set(session.id, this.generation);\n")],
     "vscode_debug_state_tests"),
    ("an ended session is changed by later events", DEBUG_STATE,
     [("        if (this.hasEnded(session.id)) {\n            return;\n        }\n        if (message?.type === 'response') {",
       "        if (message?.type === 'response') {")],
     "vscode_debug_state_tests"),
    ("the debugger's frame estimate is published as exact", DEBUG_STATE,
     [("frameCount: complete ? got.length : null,", "frameCount: total ?? (complete ? got.length : null),")],
     "vscode_debug_state_tests"),
    ("another thread continuing clears the stop", DEBUG_STATE,
     [("if (body?.allThreadsContinued === false && this.isAnotherThread(session.id, body.threadId)) {", "if (false) {")],
     "vscode_debug_state_tests"),
    ("every structured local is expanded", DEBUG_STATE,
     [("            if (expanded === EXPANDED_LIMIT) {\n", "            if (false) {\n")],
     "vscode_debug_state_tests"),
    ("a stale stop keeps asking", DEBUG_STATE,
     [("        if (isStale()) {\n", "        if (false) {\n")],
     "vscode_debug_state_tests"),
    ("a failed write leaves its temporary file", DEBUG_STATE,
     [("        fs.rmSync(temporary, { force: true });\n", "")],
     "vscode_debug_state_tests"),
    ("the file is written in place", DEBUG_STATE,
     [("        fs.writeFileSync(temporary, JSON.stringify(state, null, 1) + '\\n');\n        fs.renameSync(temporary, file);\n",
       "        fs.writeFileSync(file, JSON.stringify(state, null, 1) + '\\n');\n")],
     "vscode_debug_state_tests"),
    ("a session in any folder is published", EXTENSION,
     [("return isFillscope(session.workspaceFolder) ? session.workspaceFolder : undefined;", "return session.workspaceFolder;")],
     "vscode_debug_state_tests"),
    ("shutting down leaves the sessions as they were", EXTENSION,
     [("        publisher.stop();\n", "")],
     "vscode_debug_state_tests"),
    ("a crashed adapter's session stays", EXTENSION,
     [("                onExit: (code, signal) => publisher.sessionEnded(", "                onExit: (code, signal) => void (")],
     "vscode_debug_state_tests"),
    ("unread locals published as none", DEBUG_STATE,
     [("        details.variables = null;\n        details.variablesError = messageOf(error);",
       "        details.variables = [];\n        details.variablesError = messageOf(error);")],
     "vscode_debug_state_tests"),
    # Only the type check can catch this one: at run time the misspelt property is just undefined.
    ("a misspelt property", DEBUG_STATE,
     [("description: body?.description ?? body?.text ?? null", "description: body?.descripton ?? body?.text ?? null")],
     "vscode_debug_state_typecheck"),
]


def run(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def ctest(build_dir, tests):
    """Runs the tests matching `tests`; (passed, ran any)."""
    result = run(["ctest", "--test-dir", str(build_dir), "-R", tests])
    return result.returncode == 0, "No tests were found" not in result.stdout


def stop_on_signal(signum, _frame):
    # Raised in the main thread, so the `finally` that restores the source runs, as on Ctrl-C.
    raise SystemExit(128 + signum)


def main():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    build_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "debug"
    if run(["cmake", "--build", str(build_dir)]).returncode != 0:
        print("PROBLEM: the real code does not build")
        sys.exit(1)

    # The baseline: every set of tests a mutant names must pass on the real code. Otherwise a test
    # already failing (or flaky) would count every mutant naming it as caught.
    baseline_failures = []
    for tests in sorted({tests for _, _, _, tests in MUTANTS}):
        passed, ran = ctest(build_dir, tests)
        if not (passed and ran):
            baseline_failures.append(f"'{tests}' {'fails' if ran else 'matches no test'} on the real code")
    if baseline_failures:
        for failure in baseline_failures:
            print(f"PROBLEM: {failure}")
        print("no mutants run: the baseline must pass first")
        sys.exit(1)

    caught, problems = 0, []
    for name, file, edits, tests in MUTANTS:
        path = ROOT / file
        original = path.read_text()
        mutated = original
        try:
            for old, new in edits:
                if mutated.count(old) != 1:
                    raise LookupError(f"'{old}' occurs {mutated.count(old)} times in {file}, not once")
                mutated = mutated.replace(old, new)
            path.write_text(mutated)
            build = run(["cmake", "--build", str(build_dir)])
            if build.returncode != 0:
                problems.append(f"{name}: the mutant does not compile\n{build.stdout[-2000:]}")
                continue
            passed, ran = ctest(build_dir, tests)
            if ran and not passed:
                caught += 1
                print(f"caught:  {name}")
            else:
                problems.append(f"{name}: NOT caught by {tests}")
        except LookupError as error:
            problems.append(f"{name}: {error}")
        finally:
            path.write_text(original)
    if run(["cmake", "--build", str(build_dir)]).returncode != 0:  # back to the real code
        problems.append("the real code no longer builds after the mutants were restored")
    for problem in problems:
        print(f"PROBLEM: {problem}")
    print(f"{caught} of {len(MUTANTS)} mutants caught")
    sys.exit(0 if caught == len(MUTANTS) and not problems else 1)


main()
