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
     [("    } else if (*traced) {\n", "    } else {\n")],
     "Anchor.SelfStopWithoutATracer"),
    ("set_self_stop does nothing", ANCHOR,
     [("void set_self_stop(bool on) { g_self_stop = on; }", "void set_self_stop(bool) {}")],
     "Anchor.SelfStopStopsATracedProcess"),
    # After int3 the debugger reports the next instruction's line, which the nop keeps on the int3's
    # own line. (As first written, that was the closing brace; the function's shape has since
    # changed what GCC puts there.) A statement after it, with no nop, moves the stop to its line.
    ("the self-stop is reported past its own line", ANCHOR,
     [('asm volatile("int3\\n\\tnop" ::"r"(frame_index));',
       'asm volatile("int3" ::"r"(frame_index));\n        fillscope::g_anchored = false;')],
     "LockstepDebugger.GdbSelfStop|LockstepDebugger.LldbSelfStop"),
    ("at -O2 the self-stop can't read frame_index", ANCHOR,
     [('asm volatile("int3\\n\\tnop" ::"r"(frame_index));', 'asm volatile("int3\\n\\tnop");')],
     "LockstepDebugger.AtO2GdbSelfStop|LockstepDebugger.AtO2LldbSelfStop"),
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
    # The lockstep claim itself: frame N is sent before the stop for frame N.
    ("the anchor is called before the frame is sent", SOURCE,
     [("        if (const auto sent = send_message(*viewer, frame); !sent) {\n",
       "        fillscope_frame_anchor(frame.index);\n        if (const auto sent = send_message(*viewer, frame); !sent) {\n"),
      ("        fillscope_frame_anchor(frame.index);\n        if (i + 1 < options.frames) {",
       "        if (i + 1 < options.frames) {")],
     "LockstepDebugger.LldbBreakpointByName|LockstepDebugger.LldbSelfStop"),
    ("the frame source never waits", SOURCE,
     [("            std::this_thread::sleep_for(options.interval);  // between frames, not after the last\n", "")],
     "RunFrameSource.WaitsTheInterval"),
    ("the frame source waits after the last frame too", SOURCE,
     [("        if (i + 1 < options.frames) {", "        if (true) {")],
     "RunFrameSource.WaitsTheInterval"),
    ("draw_uniform takes an empty range", SOURCE_HEADER,
     [('    assert(low <= high && "draw_uniform: an empty range");\n', "")],
     "DrawUniformDeathTest"),
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
     [("if (!peer_closed(sender_) || datagram_waiting(sender_)) {", "if (true) {")],
     "Channel.ReceiverReportsConnectAndDisconnect"),
    ("an empty message then a close is taken for a disconnect", CHANNEL,
     [("if (!peer_closed(sender_) || datagram_waiting(sender_)) {", "if (!peer_closed(sender_)) {")],
     "Channel.AnEmptyDatagramIsAnErrorEvenWhenTheSenderThenCloses"),
    ("a second sender waits in the queue", CHANNEL,
     [("    while (sender_) {\n        if (!accept_waiting()) {\n            break;\n        }\n    }\n", "")],
     "Channel.ASecondSenderIsRefused"),
    ("a failed accept means nobody waiting", CHANNEL,
     [("    if (!nobody_to_accept(errno)) {\n", "    if (false) {\n")],
     "Channel.AFailedAcceptIsReported"),
    ("a sender is never told it was accepted", CHANNEL,
     [("            (void)::send(accepted->get(), &kAccepted, 1, MSG_NOSIGNAL | MSG_DONTWAIT);\n", "")],
     "Channel.ASenderLearns|RunFrameSource.DescribesTheRun"),
    ("a refused sender believes it was accepted", CHANNEL,
     [("            return std::unexpected{std::make_error_code(std::errc::connection_refused)};  // closed: refused",
       "            return {};")],
     "Channel.ASenderLearns|RunFrameSource.IsRefused"),
    ("the frame source sends before it is accepted", SOURCE,
     [("    if (const auto accepted = wait_until_accepted(*viewer, options.accept_timeout); !accepted) {",
       "    if (const std::expected<void, std::error_code> accepted{}; !accepted) {")],
     "RunFrameSource.SendsNothingToAViewerThatNeverAcceptsIt|RunFrameSource.IsRefused"),
    ("a gone viewer's message is garbled", SOURCE,
     [('"frame_source: the viewer went away before {} was sent"', '"frame_source: the viewer went away {}"')],
     "RunFrameSource.FailsWhenTheViewerGoesAway"),
    ("the seed is read into the frame count", SOURCE,
     [("            options.seed = *number;", "            options.frames = *number;")],
     "ParseSourceArgs"),
    ("the frame source takes an empty socket name", SOURCE,
     [('                return std::unexpected{"--socket needs a name"};', '                options.socket_name = value;')],
     "ParseSourceArgs.RejectsUnknown"),
    ("a drain takes everything waiting", CHANNEL,
     [("while (sender_ && result.received < max_per_drain_) {", "while (sender_) {")],
     "Channel.ADrainTakesAtMostItsBound"),
    ("an empty name binds a name the kernel chooses", CHANNEL,
     [("    if (name.empty()) {\n        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};\n    }\n", "")],
     "Channel.AnEmptyNameAndNoAttemptsAreRefused"),
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
     [(".pointed = published->point && published->point->frame == frame,", ".pointed = true,")],
     "viewer_ui_tests"),
    ("an old run's point marks the new run", VIEW,
     [("if (!published || published->source.run_number != run_number) {", "if (!published) {")],
     "viewer_ui_tests"),
    ("the drawn table doesn't show the point", VIEW,
     [("            report_marked(row_label, marks.pointed);", "            report_marked(row_label, false);")],
     "viewer_ui_tests"),
    ("a hello counts as a frame for the timing", "src/viewer/viewer_app.cpp",
     [("    if (std::holds_alternative<Frame>(message)) {\n", "    if (true) {\n")],
     "viewer_ui_tests"),
    ("a new source joins the last one's run", "src/viewer/viewer_app.cpp",
     [("        if (connection_pending_) {\n", "        if (false) {\n")],
     "viewer_ui_tests"),
    ("a connection that sends nothing valid wipes the run", "src/viewer/viewer_app.cpp",
     [("    if (receiver.drain(received_).new_sender) {\n        connection_pending_ = true;\n    }\n",
       "    if (receiver.drain(received_).new_sender) {\n        begin_connection();\n    }\n")],
     "viewer_ui_tests"),
    ("the receiver's error isn't recorded", "src/viewer/viewer_app.cpp",
     [("    state_.source_error = receiver.error();\n", "")],
     "viewer_ui_tests"),
    ("finish publishes nothing", "src/viewer/viewer_app.cpp",
     [("    publisher_.finish(make_context(state_, selection_));\n", "")],
     "viewer_ui_tests"),
    ("the seconds since the last frame are always zero", "src/viewer/viewer_app.cpp",
     [("std::chrono::duration<double>(now - *last_frame_at_).count()", "std::chrono::duration<double>(now - now).count()")],
     "viewer_ui_tests"),
    ("a publishing failure is never reported", "src/viewer/viewer_app.cpp",
     [("    if (error == reported_error_) {\n", "    if (true) {\n")],
     "viewer_ui_tests"),
    ("a new connection keeps the old run", VIEWER_STATE_HEADER,
     [("void begin_connection() { run.reset(); }", "void begin_connection() {}")],
     "ViewerState.ANewConnectionNeverInherits"),
    ("an id's frames are indexed under the wrong id", VIEWER_STATE,
     [("indices_by_id_[frame.id].push_back(frame.index);", "indices_by_id_[frame.id + 1].push_back(frame.index);")],
     "Run.IndexesEachIdsFrames|MakeContext.FollowLists|viewer_ui_tests"),
    ("the envelope grows without bound", VIEWER_STATE,
     [("    if (buckets_.size() <= kMaxBuckets) {\n", "    if (true) {\n")],
     "PriceEnvelope.EveryPriceStaysInTheBucket|PriceEnvelope.KeepsEveryLowAndHigh"),
    ("merging loses the low", VIEWER_STATE,
     [("return {.low = std::min(a.low, b.low)", "return {.low = a.low")],
     "PriceEnvelope.EveryPriceStaysInTheBucket|PriceEnvelope.KeepsEveryLowAndHigh"),
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
     [("        error_ = std::move(written.error());  // the bar shows this\n",
       "        error_ = std::move(written.error());\n        published_ = context;\n")],
     "Publisher.AFailedWriteKeeps|viewer_ui_tests"),
    ("a failed write leaves the old file", PUBLISHER,
     [("        if (::unlink(file_.c_str()) == 0) {", "        if (false) {")],
     "Publisher.AFailedWriteRemovesTheOldFile"),
    ("a failed write is retried only when the context changes", PUBLISHER,
     [("    if (published_ == context && !error_) {", "    if (published_ == context) {")],
     "Publisher.AFailedWriteIsRetriedWhenTheContextReturns"),
    ("a run's broken indices still give replay addresses", VIEWER_STATE,
     [("    if (!index_break_ && frame.index != frames_.size()) {", "    if (false) {")],
     "Run.HasReplayAddressesOnly|Run.ARepeatedIndex|MakeContext.IndicesThatDont"),
    ("the last bucket is counted as full after a merge", VIEWER_STATE,
     [("    buckets_[pairs] = buckets_.back();\n", "    buckets_[pairs] = buckets_.back();\n    frames_in_last_bucket_ = frames_per_bucket_ * 2;\n")],
     "PriceEnvelope.EveryPriceStaysInTheBucket"),
    ("a reduction drops its last, partial group", VIEWER_STATE,
     [("    for (std::size_t start = 0; start < buckets_.size(); start += group) {",
       "    for (std::size_t start = 0; start + group <= buckets_.size(); start += group) {")],
     "PriceEnvelope.ReducedToAnyWidth"),
    ("prices are plotted as floats", VIEWER_STATE,
     [("    prices_.append(frame.price);", "    prices_.append(static_cast<std::int64_t>(static_cast<float>(frame.price)));")],
     "Run.PlotsLargePricesExactly"),
    ("more frames than described isn't said", "src/viewer/context.h",
     [("            return run && frames_received > run->frame_count;", "            return false;")],
     "ContextBar.SaysWhenASourceSentMore"),
    ("a signal ends the viewer at once", "src/viewer/stop_signals.cpp",
     [("    action.sa_handler = request_stop;", "    action.sa_handler = SIG_DFL;")],
     "StopSignals"),
    ("an infinite --exit-after is accepted", "src/viewer/viewer_options.cpp",
     [("if (!(*seconds >= 0) || std::isinf(*seconds)) {", "if (!(*seconds >= 0)) {")],
     "ParseViewerArgs"),
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


def werror(build_dir):
    """The build's FILLSCOPE_WERROR setting, from its cache."""
    cache = (build_dir / "CMakeCache.txt").read_text()
    return "ON" if "FILLSCOPE_WERROR:BOOL=ON" in cache else "OFF"


def set_werror(build_dir, value):
    return run(["cmake", "-S", str(ROOT), "-B", str(build_dir), f"-DFILLSCOPE_WERROR={value}"]).returncode == 0


def main():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    build_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "debug"
    # A mutant is broken code, which may well draw a warning (an unused parameter, say); what is
    # checked here is the tests, so warnings don't fail a mutant's build. The setting is restored.
    previous = werror(build_dir)
    try:
        set_werror(build_dir, "OFF")
        check(build_dir)
    finally:
        set_werror(build_dir, previous)


def check(build_dir):
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
