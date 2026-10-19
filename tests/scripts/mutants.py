# Checks that the tests catch bugs: each mutant breaks the code in one deliberate way, and the
# tests named for it must then fail. The source is always restored, even on interruption.
#
#   python3 -I tests/scripts/mutants.py [BUILD_DIR]     (default build/debug)
#
# A mutant whose text no longer matches the source is an error, not a pass: the check must not go
# silently stale when the code changes.
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

ANCHOR = "src/lockstep/anchor.cpp"
CHANNEL = "src/channel/net.cpp"
SOURCE = "tools/frame_source/frame_source.cpp"
VIEW = "src/viewer/viewer_view.cpp"
MESSAGE = "src/channel/message.cpp"
CONTEXT = "src/viewer/context.cpp"
PUBLISHER = "src/viewer/context_publisher.cpp"
DOC_LINKS = "tests/docs/doc_links_test.cpp"
DEBUG_STATE = "tools/vscode-debug-state/src/debug_state.js"

# (name, file, [(old, new), ...], CTest regex of tests that must fail)
MUTANTS = [
    ("indices skip", SOURCE,
     [(".index = next_index_++,", ".index = next_index_ += 2,")],
     "FrameGenerator.IndicesAreConsecutive"),
    ("price step too large", SOURCE,
     [("step{-kMaxPriceStep, kMaxPriceStep}", "step{-kMaxPriceStep - 1, kMaxPriceStep}")],
     "FrameGenerator.PriceWalks"),
    ("any TracerPid means a debugger", ANCHOR,
     [("return error == std::errc{} && pid != 0;", "return error == std::errc{};")],
     "TracerAttached.ReadsTracerPid"),
    ("anchor doesn't record", ANCHOR,
     [("fillscope::g_last_anchor_index = frame_index;", "fillscope::g_last_anchor_index = 0;")],
     "Anchor.RecordsEachFrameIndex"),
    ("self-stop ignores the debugger check", ANCHOR,
     [("if (fillscope::g_self_stop && fillscope::debugger_attached())", "if (fillscope::g_self_stop)")],
     "Anchor.SelfStopWithoutADebugger"),
    # The bug found in VS Code, as it was: the anchor's first statement an std::atomic store,
    # inlined from libstdc++ even at -O0. LLDB's breakpoint by name lands in atomic_base.h; GDB's
    # stays in our file, so only the LLDB test can catch it.
    ("anchor's first statement inlined from a header", ANCHOR,
     [("#include <charconv>", "#include <atomic>\n#include <charconv>"),
      ("std::uint64_t g_last_anchor_index = 0;", "std::atomic<std::uint64_t> g_last_anchor_index{0};"),
      ("fillscope::g_last_anchor_index = frame_index;",
       "fillscope::g_last_anchor_index.store(frame_index, std::memory_order_release);")],
     "LockstepDebugger.LldbBreakpointByName"),
    # A unit test that becomes slow must fail as slow: the unit tier's timeout is 2 s.
    ("a unit test takes 3 seconds", ANCHOR,
     [("#include <charconv>", "#include <charconv>\n#include <thread>"),
      ('    constexpr std::string_view key = "TracerPid:";',
       '    std::this_thread::sleep_for(std::chrono::seconds{3});\n    constexpr std::string_view key = "TracerPid:";')],
     "TracerAttached.ReadsTracerPid"),
    ("frame source skips the anchor", SOURCE,
     [("        fillscope_frame_anchor(frame.index);\n", "")],
     "RunFrameSource.DescribesTheRun|LockstepDebugger"),
    ("--socket ignored", SOURCE,
     [("options.socket_name = args[++i];", "++i;")],
     "ParseSourceArgs|LockstepDebugger|viewer_ui_tests"),
    ("messages reordered on receipt", CHANNEL,
     [("out.push_back(*message);", "out.insert(out.begin(), *message);")],
     "Channel.EveryMessageSentArrivesInOrder|RunFrameSource.DescribesTheRun|viewer_ui_tests"),
    ("a datagram of the wrong size decodes", MESSAGE,
     [("if (datagram.size() != kMessageHeaderSize + sizeof(Payload)) {",
       "if (false && datagram.size() != kMessageHeaderSize + sizeof(Payload)) {")],
     "Message.DecodeRejects"),
    ("disconnect not noticed", CHANNEL,
     [("sender_.reset();  // 0:", "break;  // 0:")],
     "Channel.ReceiverReportsConnectAndDisconnect|Channel.ReceiverAcceptsTheNextSender"),
    ("frame count off by one", VIEW,
     [('text(std::format("frames received: {}", received));', 'text(std::format("frames received: {}", received + 1));')],
     "viewer_ui_tests"),
    ("always shows connected", VIEW,
     [('state.source_connected ? "connected" : "waiting"', '"connected"')],
     "viewer_ui_tests"),
    ("last frame's index off by one", VIEW,
     [("price {}  qty {}\", last.index, last.id", "price {}  qty {}\", last.index + 1, last.id")],
     "viewer_ui_tests"),
    ("plot and table hidden until many frames", VIEW,
     [("if (state.run && !state.run->prices().empty()) {", "if (state.run && state.run->prices().size() > 5) {")],
     "viewer_ui_tests"),
    ("clicking an id also points at its frame", VIEW,
     [("                actions.follow = frame.id;\n",
       "                actions.follow = frame.id;\n                actions.point_at = frame;\n")],
     "viewer_ui_tests"),
    ("follow lists the frames without the id", CONTEXT,
     [("return f.id == *id;", "return f.id != *id;")],
     "MakeContext.FollowLists|viewer_ui_tests"),
    ("a new run keeps the old point", CONTEXT,
     [("    if (pointed_) {\n        clear_point();\n    }\n", "")],
     "Selection.ANewRunClearsThePoint"),
    ("the replay address names the wrong frame", CONTEXT,
     [(".frame_index = frame->index", ".frame_index = 0")],
     "MakeContext.ThePointHasAReplayAddress|viewer_ui_tests"),
    ("a user action waits like a background change", PUBLISHER,
     [("if (!user_action && last_attempt_ && now - *last_attempt_ < min_interval_) {",
       "if (last_attempt_ && now - *last_attempt_ < min_interval_) {")],
     "Publisher.AUserActionIsWrittenAtOnce"),
    ("an unchanged context is rewritten", PUBLISHER,
     [("    if (published_ == context && !error_) {\n        return;  // the file already says exactly this\n    }\n", "")],
     "Publisher.AnUnchangedContextIsNotRewritten"),
    # The bar draws what was published; claiming a failed write as published would make it lie.
    ("a failed write is claimed as published", PUBLISHER,
     [("        error_ = std::move(written.error());  // the bar shows this; published_ still says what the file holds\n"
       "        return;\n",
       "        error_ = std::move(written.error());\n        published_ = context;\n        return;\n")],
     "Publisher.AFailedWriteKeeps|viewer_ui_tests"),
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
    ("resuming requests are ignored", DEBUG_STATE,
     [("if (message?.type === 'request' && RESUMING_REQUESTS.has(message.command)) {",
       "if (message?.type === 'request' && message.command === 'continue') {")],
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


def main():
    build_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "debug"
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
            result = run(["ctest", "--test-dir", str(build_dir), "-R", tests])
            if result.returncode != 0 and "No tests were found" not in result.stdout:
                caught += 1
                print(f"caught:  {name}")
            else:
                problems.append(f"{name}: NOT caught by {tests}")
        except LookupError as error:
            problems.append(f"{name}: {error}")
        finally:
            path.write_text(original)
    run(["cmake", "--build", str(build_dir)])  # back to the real code
    for problem in problems:
        print(f"PROBLEM: {problem}")
    print(f"{caught} of {len(MUTANTS)} mutants caught")
    sys.exit(0 if caught == len(MUTANTS) else 1)


main()
