# Checks that the tests catch bugs: each mutant breaks the code in one deliberate way, and the
# tests named for it must then fail, though they pass on the real code (checked first). It works in
# a scratch copy of the working tree with a build of its own (scratch_tree.py), so the files being
# edited and the build in use are never touched, whatever stops it.
#
#   python3 -I tests/scripts/mutants.py [NAME]     every mutant, or those whose names contain NAME
#
# A mutant whose text no longer matches the source is an error, not a pass: the check must not go
# silently stale when the code changes.
import importlib.util
import pathlib
import re
import shutil
import signal
import subprocess
import sys

# Loading the scripts beside this one by path would otherwise leave __pycache__ files in the tree.
sys.dont_write_bytecode = True

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
ATTRIBUTION = "tools/attribution/attribution.py"

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
     "ParseSourceArgs|LockstepDebugger + ui:a_real_source_process_describes_its_run_and_sends_frames,a_new_source_never_inherits_the_last_ones_run,a_connection_that_sends_nothing_valid_leaves_the_run_in_place"),
    ("messages reordered on receipt", CHANNEL,
     [("out.push_back(*message);", "out.insert(out.begin(), *message);")],
     "Channel.EveryMessageSentArrivesInOrder|RunFrameSource.DescribesTheRun + ui:a_real_source_process_describes_its_run_and_sends_frames,a_new_source_never_inherits_the_last_ones_run,a_connection_that_sends_nothing_valid_leaves_the_run_in_place"),
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
    ("a NaN --exit-after is accepted", "src/viewer/viewer_options.cpp",
     [("if (!(*seconds >= 0) || std::isinf(*seconds)) {", "if (*seconds < 0 || std::isinf(*seconds)) {")],
     "ParseViewerArgs.RefusesWhatIsntAnOption"),
    ("an empty --context-file is accepted", "src/viewer/viewer_options.cpp",
     [('                return std::unexpected{"--context-file needs a path"};', '                options.context_file = "";')],
     "ParseViewerArgs.RefusesWhatIsntAnOption"),
    ("the frame source's bad command line exits 1", "tools/frame_source/main.cpp",
     [("        return 2;", "        return 1;")],
     "FrameSourceProgram.RefusesABadCommandLine"),
    # The lockstep driver counts what arrived at each stop; a miscount must fail the lockstep check.
    ("the LLDB driver miscounts the messages", "tests/lockstep/lldb_driver.py",
     [("                self.received += 1", "                self.received += 2")],
     "LockstepDebugger.LldbSelfStop|LockstepDebugger.LldbBreakpointByName"),
    # Each tier's timeout fails a program that grows too slow (the unit tier's has its own mutant).
    ("a UI test takes 11 seconds", "tests/viewer/viewer_ui_test.cpp",
     [("int main(int argc, char** argv) {\n", "int main(int argc, char** argv) {\n    std::this_thread::sleep_for(std::chrono::seconds{11});\n"),
      ("#include <deque>\n", "#include <deque>\n#include <thread>\n")],
     "viewer_ui_tests"),
    ("the type check takes 31 seconds", "tests/vscode-debug-state/CMakeLists.txt",
     [("  COMMAND ${FLOCK} --shared ${EXTENSION} ${NODE} ${EXTENSION}/node_modules/typescript/bin/tsc -p ${EXTENSION}",
       "  COMMAND ${CMAKE_COMMAND} -E sleep 31")],
     "vscode_debug_state_typecheck"),
    ("a debugger test takes 31 seconds", "tests/lockstep/lldb_driver.py",
     [("MAX_STOPS = 1000", "import time\ntime.sleep(31)\nMAX_STOPS = 1000")],
     "LockstepDebugger.LldbDriverNamesTheSignal"),
    ("a program with no tests passes registration", "tests/scripts/iterate.py",
     [("        if not with_tests.get(program.name):", "        if False:")],
     "scripts_tests"),
    ("a test that copies a program's labels passes registration", "tests/scripts/iterate.py",
     [("        elif program.binary is not None and program.binary not in command:", "        elif False:")],
     "scripts_tests"),
    ("a program that tested nothing is recorded", "tests/scripts/iterate.py",
     [("    return {p.name for p in everything if not by_program.get(p.name) or set(by_program[p.name]) <= skipped}",
       "    return {p.name for p in everything if by_program.get(p.name) and set(by_program[p.name]) <= skipped}")],
     "scripts_tests"),
    ("the sanitizer check ignores UBSan", "tests/scripts/iterate.py",
     [('        for library in ("libasan", "libubsan"):', '        for library in ("libasan",):')],
     "scripts_tests"),
    ("a deleted run-time file crashes the fingerprint", "tests/scripts/iterate.py",
     [("    if not file.exists():\n        return b\"missing\"\n", "")],
     "scripts_tests"),
    ("blame credits a whitespace-only change", ATTRIBUTION,
     [('"blame", "--porcelain", "-w", "-M", "-C",', '"blame", "--porcelain", "-M", "-C",')],
     "attribution_tests"),
    ("blame doesn't follow lines moved between files", ATTRIBUTION,
     [('"blame", "--porcelain", "-w", "-M", "-C",', '"blame", "--porcelain", "-w",')],
     "attribution_tests"),
    ("a co-author whose name starts with Claude counts as Claude", ATTRIBUTION,
     [('    return value == "Claude" or value.startswith("Claude ")', '    return value.startswith("Claude")')],
     "attribution_tests"),
    ("binary files are counted", ATTRIBUTION,
     [('        if added != "-" and path in regular:', '        if path in regular:')],
     "attribution_tests"),
    ("the report counts itself", ATTRIBUTION,
     [("        if path == REPORT:\n            continue\n", "")],
     "attribution_tests"),
    ("files nobody wrote are counted", ATTRIBUTION,
     [("        if path in NOT_WRITTEN:", "        if False:")],
     "attribution_tests"),
    ("the check accepts a commit outside HEAD's history", ATTRIBUTION,
     [("    if ancestor.returncode != 0:", "    if False:")],
     "attribution_tests"),
    ("the check doesn't compare the numbers", ATTRIBUTION,
     [("    if report(root, commit) != text:", "    if False:")],
     "attribution_tests"),
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
     "ui:shows_counts_and_last_frame,shows_waiting_without_source,a_hello_without_frames_shows_no_plot_or_table"),
    ("always shows connected", VIEW,
     [("    if (state.source_connected) {\n", "    if (true) {\n")],
     "ui:shows_waiting_without_source,says_why_the_last_source_was_dropped,a_connection_that_sends_nothing_valid_leaves_the_run_in_place"),
    ("last frame's index off by one", VIEW,
     [("price {}  qty {}\", last.index, last.id", "price {}  qty {}\", last.index + 1, last.id")],
     "ui:shows_counts_and_last_frame,a_new_run_drops_the_point_and_keeps_the_follow"),
    ("plot and table hidden until many frames", VIEW,
     [("if (view.state.run && !view.state.run->frames().empty()) {",
       "if (view.state.run && view.state.run->frames().size() > 5) {")],
     "ui:plot_and_table_appear_with_frames"),
    ("clicking an id also points at its frame", VIEW,
     [("                actions.follow = frame.id;\n",
       "                actions.follow = frame.id;\n                actions.point_at = frame;\n")],
     "ui:clicking_an_id_follows_it,the_bar_says_exactly_what_the_file_holds"),
    # The table's marks, like the bar, must say what was published, not what the user clicked.
    ("the table marks the live selection", VIEW,
     [(".pointed = published->point && published->point->frame == frame,", ".pointed = true,")],
     "ui:the_bar_says_exactly_what_the_file_holds,a_new_run_drops_the_point_and_keeps_the_follow"),
    ("an old run's point marks the new run", VIEW,
     [("if (!published || published->source.run_number != run_number) {", "if (!published) {")],
     "ui:the_old_runs_published_point_marks_nothing_in_a_new_run"),
    ("the drawn table doesn't show the point", VIEW,
     [("            report_marked(row_label, marks.pointed);", "            report_marked(row_label, false);")],
     "ui:the_bar_says_exactly_what_the_file_holds"),
    ("a hello counts as a frame for the timing", "src/viewer/viewer_app.cpp",
     [("    if (std::holds_alternative<Frame>(message)) {\n", "    if (true) {\n")],
     "ui:a_hello_without_frames_shows_no_plot_or_table"),
    ("a new source joins the last one's run", "src/viewer/viewer_app.cpp",
     [("        if (connection_pending_) {\n", "        if (false) {\n")],
     "ui:a_new_source_never_inherits_the_last_ones_run"),
    ("a connection that sends nothing valid wipes the run", "src/viewer/viewer_app.cpp",
     [("    if (receiver.drain(received_).new_sender) {\n        connection_pending_ = true;\n    }\n",
       "    if (receiver.drain(received_).new_sender) {\n        begin_connection();\n    }\n")],
     "ui:a_connection_that_sends_nothing_valid_leaves_the_run_in_place"),
    ("the receiver's error isn't recorded", "src/viewer/viewer_app.cpp",
     [("    state_.source_error = receiver.error();\n", "")],
     "ui:a_connection_that_sends_nothing_valid_leaves_the_run_in_place"),
    ("finish publishes nothing", "src/viewer/viewer_app.cpp",
     [("    publisher_.finish(make_context(state_, selection_));\n", "")],
     "ui:finish_publishes_that_the_viewer_has_stopped"),
    ("the seconds since the last frame are always zero", "src/viewer/viewer_app.cpp",
     [("std::chrono::duration<double>(now - *last_frame_at_).count()", "std::chrono::duration<double>(now - now).count()")],
     "ui:the_seconds_since_the_last_frame_count_from_it"),
    ("a publishing failure is never reported", "src/viewer/viewer_app.cpp",
     [("    if (error == reported_error_) {\n", "    if (true) {\n")],
     "ui:publishing_failing_and_recovering_is_reported_once_each"),
    ("a new connection keeps the old run", VIEWER_STATE_HEADER,
     [("void begin_connection() { run.reset(); }", "void begin_connection() {}")],
     "ViewerState.ANewConnectionNeverInherits"),
    ("an id's frames are indexed under the wrong id", VIEWER_STATE,
     [("indices_by_id_[frame.id].push_back(frame.index);", "indices_by_id_[frame.id + 1].push_back(frame.index);")],
     "Run.IndexesEachIdsFrames|MakeContext.FollowLists + ui:clicking_an_id_follows_it,a_new_run_drops_the_point_and_keeps_the_follow"),
    ("the envelope grows without bound", VIEWER_STATE,
     [("    if (buckets_.size() <= kMaxBuckets) {\n", "    if (true) {\n")],
     "PriceEnvelope.EveryPriceStaysInTheBucket|PriceEnvelope.KeepsEveryLowAndHigh"),
    ("merging loses the low", VIEWER_STATE,
     [("return {.low = std::min(a.low, b.low)", "return {.low = a.low")],
     "PriceEnvelope.EveryPriceStaysInTheBucket|PriceEnvelope.KeepsEveryLowAndHigh"),
    ("a new run keeps the old point", CONTEXT,
     [("    pointed_.reset();  // point_seq_ unchanged: the user didn't do this\n", "")],
     "Selection.ANewRunClearsThePoint + ui:a_new_run_drops_the_point_and_keeps_the_follow,a_new_source_never_inherits_the_last_ones_run"),
    ("a new run takes a sequence number", CONTEXT,
     [("    pointed_.reset();  // point_seq_ unchanged: the user didn't do this\n",
       "    clear_point();\n")],
     "Selection.ANewRunTakesNoSequenceNumber + ui:a_new_run_drops_the_point_and_keeps_the_follow"),
    ("the replay address names the wrong frame", CONTEXT,
     [(".frame_index = frame->index", ".frame_index = 0")],
     "MakeContext.ThePointHasAReplayAddress + ui:clicking_a_row_points_at_its_frame"),
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
     "Publisher.AFailedWriteKeeps + ui:a_failed_write_shows_in_the_bar_and_the_table,the_old_runs_published_point_marks_nothing_in_a_new_run"),
    ("a failed write leaves the old file", PUBLISHER,
     [("        if (::unlink(file_.c_str()) == 0) {", "        if (false) {")],
     "Publisher.AFailedWriteRemovesTheOldFile + node:a failed write removes the old file, is logged, and the next write works again"),
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
     "Publisher.AFailedWriteIsRetried + node:a failed write throws and leaves no temporary file"),
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
     [("            if (isStale()) {\n                return;  // the program moved on", "            if (false) {\n                return;  // the program moved on")],
     "node:a stop whose details arrive after the program resumed is not published, and asks nothing more|of two overlapping stops, only the later is published|a session that ends while a stop is being read stays ended"),
    ("only 'continue' resumes", DEBUG_STATE,
     [("        if (RESUMING_REQUESTS.has(request.command) || MOVING_REQUESTS.has(request.command)",
       "        if (request.command === 'continue' || MOVING_REQUESTS.has(request.command)")],
     "node:every resuming request publishes running once the debugger accepts it, not when it is sent|stepping another thread alone leaves the published stop|a stop whose details arrive after the program resumed is not published, and asks nothing more"),
    ("a refused resume is published as running", DEBUG_STATE,
     [("        if (!response.success) {\n", "        if (false) {\n")],
     "node:a refused resuming request leaves the stop published|a launch that fails leaves the session starting, never running"),
    ("a late resume hides a newer stop", DEBUG_STATE,
     [("        if (record.generation !== pending.generation) {\n", "        if (false) {\n")],
     "node:a resume accepted after a newer stop arrived does not hide that stop"),
    ("an earlier incarnation's calls are obeyed", DEBUG_STATE,
     [("        return incarnation === undefined || this.records.get(id)?.incarnation === incarnation;", "        return true;")],
     "node:calls from a session's earlier incarnation change nothing|after a restart under the same id, the old adapter's late exit and messages change nothing"),
    ("an ended session is changed by later events", DEBUG_STATE,
     [("        if (record === undefined || this.hasEnded(session.id) || !this.isCurrent(session.id, incarnation)) {\n            return;\n        }\n        if (message?.type === 'response') {",
       "        if (record === undefined) {\n            return;\n        }\n        if (message?.type === 'response') {")],
     "node:nothing changes a session after it has ended|calls from a session's earlier incarnation change nothing|after a restart under the same id, the old adapter's late exit and messages change nothing"),
    ("the debugger's frame estimate decides completeness", DEBUG_STATE,
     [("    const complete = got < asked;", "    const complete = false;")],
     "node:a stop publishes the stack, the locals and the stop reason|an empty stack says there were no locals to read|a stack returned whole has an exact count, even when the debugger's total says more"),
    ("another thread continuing clears the stop", DEBUG_STATE,
     [("if (body?.allThreadsContinued === false && this.isAnotherThread(session.id, body.threadId)) {", "if (false) {")],
     "node:another thread continuing alone leaves the published stop; the stopped thread continuing clears it|a stop being read survives another thread resuming alone|a stop that names no thread publishes whose stack it shows, and keeps it against another thread's resume"),
    ("the stop being read isn't counted as the stopped thread", DEBUG_STATE,
     [("        if (record?.describing && record.describing.generation === record.generation) {", "        if (false) {")],
     "node:a stop being read survives another thread resuming alone"),
    ("continue's response is taken for every thread", DEBUG_STATE,
     [("if (pending.command === 'continue' && allContinued === false && this.isAnotherThread(session.id, pending.threadId)) {",
       "if (false) {")],
     "node:a continue the debugger says resumed only another thread leaves the stop"),
    ("a thread-less stop's thread isn't the stopped one", DEBUG_STATE,
     [("                stopped = entry.framesThreadId ?? entry.stop.threadId;", "                stopped = entry.stop.threadId;")],
     "node:a stop that names no thread publishes whose stack it shows, and keeps it against another thread's resume"),
    ("goto is published as running", DEBUG_STATE,
     [("            this.change(session, { state: 'moving' });", "            this.change(session, { state: 'running' });")],
     "node:goto and restartFrame make the stop out of date without claiming the program runs"),
    ("terminated is published as exited", DEBUG_STATE,
     [("                this.change(session, { state: 'terminated' });", "                this.change(session, { state: 'exited', exitCode: null });")],
     "node:\"terminated\" says debugging ended, not that the program exited"),
    ("a session is running before its launch succeeds", DEBUG_STATE,
     [("        this.publish(session, { state: 'starting' });", "        this.publish(session, { state: 'running' });")],
     "node:a session that ends is published as ended, and dropped when a new session starts|a session is starting until its launch is accepted, then running|a launch that fails leaves the session starting, never running"),
    ("an expensive scope named Locals is read", DEBUG_STATE,
     [("    const cheap = scopes.filter((s) => !s.expensive);", "    const cheap = scopes;")],
     "node:without a scope named for locals, the first inexpensive one is read, never an expensive one|the locals are the scope the debugger marks as such, else the one named so, never an expensive one"),
    ("any scope containing 'local' is the locals", DEBUG_STATE,
     [("        ?? cheap.find((s) => /^locals?$/i.test(s.name))", "        ?? cheap.find((s) => /local/i.test(s.name))")],
     "node:the locals are the scope the debugger marks as such, else the one named so, never an expensive one"),
    ("a stop's text is published as its description", DEBUG_STATE,
     [("            description: body?.description ?? null,", "            description: body?.description ?? body?.text ?? null,")],
     "node:a stop's text and description are published as the debugger gave them"),
    ("every structured local is expanded", DEBUG_STATE,
     [("            if (expanded === EXPANDED_LIMIT) {\n", "            if (false) {\n")],
     "node:only the first few structured locals are expanded, and the rest say so"),
    ("a stale stop keeps asking", DEBUG_STATE,
     [("    const ask = async (command, args) => {\n        if (isStale()) {\n", "    const ask = async (command, args) => {\n        if (false) {\n")],
     "node:a stop whose details arrive after the program resumed is not published, and asks nothing more|a stop that goes stale while members are being read asks nothing more|every write by the writer has a higher sequence number, its last one too"),
    ("stop() doesn't take a new sequence number", DEBUG_STATE,
     [("    stop() {\n        this.seq += 1;\n", "    stop() {\n")],
     "node:the output line names the session, the place and the counts|every write by the writer has a higher sequence number, its last one too"),
    ("a failed write leaves its temporary file", DEBUG_STATE,
     [("        io.rmSync(temporary, { force: true });\n", "")],
     "node:a failed write throws and leaves no temporary file"),
    # Caught by a test that watches the calls, so it doesn't depend on a race with a reader.
    ("the file is written in place", DEBUG_STATE,
     [("        io.writeFileSync(temporary, JSON.stringify(state, null, 1) + '\\n');\n        io.renameSync(temporary, file);\n",
       "        io.writeFileSync(file, JSON.stringify(state, null, 1) + '\\n');\n")],
     "node:the file is written to a temporary file in its directory, then renamed over the old one|a reader never sees part of a write|a failed write removes the old file, is logged, and the next write works again"),
    ("a session in any folder is published", EXTENSION,
     [("return isFillscope(session.workspaceFolder) ? session.workspaceFolder : undefined;", "return session.workspaceFolder;")],
     "node:a session outside a Fillscope folder gets no tracker and writes nothing|in a workspace with a Fillscope folder, a session of another folder is still not published"),
    ("a session of another folder goes to the Fillscope folder", EXTENSION,
     [("    if (session.workspaceFolder !== undefined) {\n        return isFillscope(session.workspaceFolder) ? session.workspaceFolder : undefined;\n    }\n", "")],
     "node:in a workspace with a Fillscope folder, a session of another folder is still not published|each Fillscope folder has its own file, including one added after activation"),
    ("shutting down leaves the sessions as they were", EXTENSION,
     [("        publisher.stop();\n", "")],
     "node:when VS Code shuts the extension down, the file says so"),
    ("a normal adapter exit is published as a failure", EXTENSION,
     [("    if (code === 0) {\n        return null;\n    }\n", "")],
     "node:an adapter process that exits says why only when it didn't end normally"),
    ("a session ended by VS Code otherwise stays", EXTENSION,
     [("            entry.publisher.sessionEnded(session.id, null, entry.incarnation);\n", "")],
     "node:a session VS Code ends any other way is ended too"),
    ("a restarted session's old adapter is obeyed", EXTENSION,
     [("                onExit: (code) => publisher.sessionEnded(session.id, adapterExitReason(code), incarnation),",
       "                onExit: (code) => publisher.sessionEnded(session.id, adapterExitReason(code)),")],
     "node:after a restart under the same id, the old adapter's late exit and messages change nothing"),
    ("a failed write leaves the old file", EXTENSION,
     [("                fs.rmSync(file, { force: true });\n", "")],
     "node:a failed write removes the old file, is logged, and the next write works again"),
    ("a symlinked folder gets a publisher of its own", EXTENSION,
     [("        const key = fs.realpathSync(folder.uri.fsPath);", "        const key = folder.uri.fsPath;")],
     "node:two workspace folders that reach one directory share one file"),
    ("unread locals published as none", DEBUG_STATE,
     [("        details.variables = null;\n        details.variablesError = messageOf(error);",
       "        details.variables = [];\n        details.variablesError = messageOf(error);")],
     "node:locals that could not be read are null with a reason, never an empty list|failed scopes leave the locals unread, with the reason"),
    # Only the type check can catch this one: at run time the misspelt property is just undefined.
    ("a misspelt property", DEBUG_STATE,
     [("            text: body?.text ?? null,", "            text: body?.txt ?? null,")],
     "vscode_debug_state_typecheck"),
]


def run(args, cwd=None):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=cwd)


def load_scratch_tree():
    """scratch_tree.py, beside this script. Loaded by path: python3 -I puts no script directory on
    the import path."""
    spec = importlib.util.spec_from_file_location("scratch_tree", pathlib.Path(__file__).with_name("scratch_tree.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Runner:
    """Runs the tests a mutant names, in the scratch copy's build. A name is one of:
      a CTest regular expression, as `ctest -R` takes;
      "ui:FILTER", Dear ImGui Test Engine tests, as viewer_ui_tests takes a filter (comma-separated
        parts of test names);
      "node:PATTERN", the extension's Node tests whose names match, as `node --test-name-pattern`
        takes;
    and several, joined by " + ", are all run. Each check returns (passed, ran any)."""

    def __init__(self, copy, build):
        self.copy = copy
        self.build = build
        self.node = shutil.which("node") or "node"
        self.node_tests = sorted(str(p) for p in (copy / "tests" / "vscode-debug-state").glob("*.test.js"))

    def check(self, spec):
        results = [self.check_one(part.strip()) for part in spec.split(" + ")]
        return all(passed for passed, _ in results), all(ran for _, ran in results)

    def check_one(self, spec):
        if spec.startswith("ui:"):
            result = run([str(self.build / "tests" / "viewer" / "viewer_ui_tests"), spec[len("ui:"):]], cwd=self.build)
            ran = re.search(r"passed of ([1-9]\d*) run", result.stdout) is not None
            return result.returncode == 0, ran
        if spec.startswith("node:"):
            result = run([self.node, "--test", f"--test-name-pattern={spec[len('node:'):]}", *self.node_tests])
            tests = re.search(r"^# tests (\d+)", result.stdout, re.MULTILINE)
            failed = re.search(r"^# fail (\d+)", result.stdout, re.MULTILINE)
            ran = tests is not None and int(tests.group(1)) > 0
            return result.returncode == 0 and failed is not None and failed.group(1) == "0", ran
        result = run(["ctest", "--test-dir", str(self.build), "-R", spec])
        return result.returncode == 0, "No tests were found" not in result.stdout


def stop_on_signal(signum, _frame):
    # Raised in the main thread, so the `finally` that removes the copy runs, as on Ctrl-C.
    raise SystemExit(128 + signum)


def main():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    only = sys.argv[1] if len(sys.argv) > 1 else None
    mutants = [m for m in MUTANTS if only is None or only in m[0]]
    scratch_tree = load_scratch_tree()
    copy = scratch_tree.make(ROOT)
    try:
        build = scratch_tree.configure(copy, ROOT)
        if build is None:
            print("PROBLEM: the copy of the working tree couldn't be configured")
            sys.exit(1)
        check(Runner(copy, build), copy, build, mutants)
    finally:
        scratch_tree.remove(copy)


def check(runner, copy, build, mutants):
    if run(["cmake", "--build", str(build)]).returncode != 0:
        print("PROBLEM: the real code does not build")
        sys.exit(1)

    # The baseline: every set of tests a mutant names must pass on the real code. Otherwise a test
    # already failing (or flaky) would count every mutant naming it as caught.
    baseline_failures = []
    for tests in sorted({tests for _, _, _, tests in mutants}):
        passed, ran = runner.check(tests)
        if not (passed and ran):
            baseline_failures.append(f"'{tests}' {'fails' if ran else 'matches no test'} on the real code")
    if baseline_failures:
        for failure in baseline_failures:
            print(f"PROBLEM: {failure}")
        print("no mutants run: the baseline must pass first")
        sys.exit(1)

    caught, problems = 0, []
    for name, file, edits, tests in mutants:
        path = copy / file
        original = path.read_text()
        mutated = original
        try:
            for old, new in edits:
                if mutated.count(old) != 1:
                    raise LookupError(f"'{old}' occurs {mutated.count(old)} times in {file}, not once")
                mutated = mutated.replace(old, new)
            path.write_text(mutated)
            built = run(["cmake", "--build", str(build)])
            if built.returncode != 0:
                problems.append(f"{name}: the mutant does not compile\n{built.stdout[-2000:]}")
                continue
            passed, ran = runner.check(tests)
            if ran and not passed:
                caught += 1
                print(f"caught:  {name}")
            else:
                problems.append(f"{name}: NOT caught by {tests}")
        except LookupError as error:
            problems.append(f"{name}: {error}")
        finally:
            path.write_text(original)
    for problem in problems:
        print(f"PROBLEM: {problem}")
    print(f"{caught} of {len(mutants)} mutants caught")
    sys.exit(0 if caught == len(mutants) and not problems else 1)


main()
