# Checks that the tests catch bugs: each mutant breaks the code in one deliberate way, and the
# tests named for it must then fail. The source is always restored, even on interruption.
#
#   python3 -I tests/mutants.py [BUILD_DIR]     (default build/debug)
#
# A mutant whose text no longer matches the source is an error, not a pass: the check must not go
# silently stale when the code changes.
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

ANCHOR = "src/lockstep/anchor.cpp"
CHANNEL = "src/channel/net.cpp"
SOURCE = "tools/frame_source/frame_source.cpp"
VIEW = "src/viewer/viewer_view.cpp"

# (name, file, [(old, new), ...], CTest regex of tests that must fail)
MUTANTS = [
    ("indices skip", SOURCE,
     [("Frame{next_index_++,", "Frame{next_index_ += 2,")],
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
     "RunFrameSource.SendsEveryGeneratedFrame|LockstepDebugger"),
    ("--socket ignored", SOURCE,
     [("options.socket_name = args[++i];", "++i;")],
     "ParseSourceArgs|LockstepDebugger|viewer_ui_tests"),
    ("frames reordered on receipt", CHANNEL,
     [("out.push_back(frame);", "out.insert(out.begin(), frame);")],
     "Channel.EveryFrameSentArrivesInOrder|RunFrameSource.SendsEveryGeneratedFrame|viewer_ui_tests"),
    ("disconnect not noticed", CHANNEL,
     [("sender_.reset();  // 0:", "break;  // 0:")],
     "Channel.ReceiverReportsConnectAndDisconnect|Channel.ReceiverAcceptsTheNextSender"),
    ("frame count off by one", VIEW,
     [("state.frames.size());", "state.frames.size() + 1);")],
     "viewer_ui_tests"),
    ("always shows connected", VIEW,
     [('state.source_connected ? "connected" : "waiting"', '"connected"')],
     "viewer_ui_tests"),
    ("last frame's index off by one", VIEW,
     [("static_cast<unsigned long long>(last.index)", "static_cast<unsigned long long>(last.index + 1)")],
     "viewer_ui_tests"),
    ("plot hidden until many frames", VIEW,
     [("if (!state.prices.empty()) {", "if (state.prices.size() > 5) {")],
     "viewer_ui_tests"),
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
