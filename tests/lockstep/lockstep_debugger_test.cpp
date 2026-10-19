// The lockstep anchor under real debuggers, driven through the frame source (the program that calls
// it): every stop is in the anchor, in our own source file, with the right frame index, and
// Continue runs on to the next frame. Each test is skipped if its debugger isn't installed. Kept
// apart from lockstep_test.cpp because these are slow (the debugger tier).
//
// FILLSCOPE_FRAME_SOURCE (the frame source binary) and FILLSCOPE_LLDB_SCRIPT (lldb_self_stop.py)
// come from the build.

#include "channel/net.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <regex>

namespace fillscope {
namespace {

constexpr int kFrames = 3;

struct Stop {
    int frame_index;
    std::string file;  // the source file's name, without its directory
};

// Every match of `pattern` in `output`; group 1 is the frame index, group 2 the source path.
std::vector<Stop> find_stops(const std::string& output, const std::regex& pattern) {
    std::vector<Stop> stops;
    for (auto it = std::sregex_iterator(output.begin(), output.end(), pattern); it != std::sregex_iterator(); ++it) {
        stops.push_back({std::stoi((*it)[1]), std::filesystem::path((*it)[2].str()).filename().string()});
    }
    return stops;
}

void expect_one_stop_per_frame_in_the_anchor(const std::vector<Stop>& stops, const std::string& output) {
    ASSERT_EQ(stops.size(), static_cast<std::size_t>(kFrames)) << output;
    for (int i = 0; i < kFrames; ++i) {
        EXPECT_EQ(stops[i].frame_index, i) << output;
        EXPECT_EQ(stops[i].file, "anchor.cpp") << "the stop should open our own source\n" << output;
    }
}

// Skips the test when `debugger` isn't installed, or fails it when FILLSCOPE_REQUIRE_DEBUGGERS is set
// (as CI sets it, so a missing debugger can't pass as a skipped test).
#define REQUIRE_DEBUGGER(debugger)                                          \
    if (!test::command_exists(debugger)) {                                  \
        if (std::getenv("FILLSCOPE_REQUIRE_DEBUGGERS") != nullptr) {        \
            FAIL() << debugger " not installed, and this run requires it"; \
        }                                                                   \
        GTEST_SKIP() << debugger " not installed";                          \
    }

// A listener for the frame source to send to, and the frame source's arguments pointing at it. The
// frames are drained afterwards, to check every one arrived.
class LockstepDebugger : public ::testing::Test {
protected:
    void SetUp() override {
        // LeakSanitizer can't run under ptrace; without this the sanitizer build's frame source
        // fails at exit.
        ::setenv("ASAN_OPTIONS", "detect_leaks=0", 1);
    }

    std::string source_args() const {
        return "--socket " + name_ + " --frames " + std::to_string(kFrames) + " --interval-ms 0";
    }

    void expect_all_frames_arrived() {
        std::vector<Frame> frames;
        receiver_.drain(frames);
        EXPECT_EQ(frames.size(), static_cast<std::size_t>(kFrames));
    }

    std::string name_ = test::unique_socket_name();
    FrameReceiver receiver_{listen_on(name_)};
};

TEST_F(LockstepDebugger, GdbBreakpointByNameStopsInTheAnchor) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = test::run_command(
        "gdb -batch -nx -ex 'break fillscope_frame_anchor' -ex 'run " + source_args() +
        "' -ex continue -ex continue -ex continue " FILLSCOPE_FRAME_SOURCE);
    expect_one_stop_per_frame_in_the_anchor(
        find_stops(output, std::regex{R"(Breakpoint 1, fillscope_frame_anchor \(frame_index=(\d+)\) at (\S+):\d+)"}),
        output);
    EXPECT_NE(output.find("exited normally"), std::string::npos) << output;
    expect_all_frames_arrived();
}

TEST_F(LockstepDebugger, GdbSelfStopStopsInTheAnchor) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = test::run_command("gdb -batch -nx -ex 'run --self-stop " + source_args() +
                                                 "' -ex continue -ex continue -ex continue " FILLSCOPE_FRAME_SOURCE);
    expect_one_stop_per_frame_in_the_anchor(
        find_stops(output, std::regex{R"(SIGTRAP[^\n]*\n(?:0x[0-9a-f]+ in )?fillscope_frame_anchor \(frame_index=(\d+)\) at (\S+):\d+)"}),
        output);
    EXPECT_NE(output.find("exited normally"), std::string::npos) << output;
    expect_all_frames_arrived();
}

TEST_F(LockstepDebugger, LldbBreakpointByNameStopsInTheAnchor) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = test::run_command(
        "lldb --batch -o 'breakpoint set -n fillscope_frame_anchor' -o 'run " + source_args() +
        "' -o continue -o continue -o continue " FILLSCOPE_FRAME_SOURCE);
    expect_one_stop_per_frame_in_the_anchor(
        find_stops(output, std::regex{R"(stop reason = breakpoint[^\n]*\n\s*frame #0: [^\n]*fillscope_frame_anchor\(frame_index=(\d+)\) at ([^:\s]+):\d+)"}),
        output);
    EXPECT_NE(output.find("exited with status = 0"), std::string::npos) << output;
    expect_all_frames_arrived();
}

// LLDB's batch mode treats a stop by signal as a crash and runs no further commands, so the
// self-stop test drives LLDB through its Python API instead.
TEST_F(LockstepDebugger, LldbSelfStopStopsInTheAnchor) {
    REQUIRE_DEBUGGER("lldb");
    std::string python_path = test::run_command("lldb -P");
    python_path.erase(python_path.find_last_not_of('\n') + 1);
    const std::string output = test::run_command("PYTHONPATH='" + python_path + "' python3 -I " FILLSCOPE_LLDB_SCRIPT
                                                 " " FILLSCOPE_FRAME_SOURCE " --self-stop " + source_args());
    expect_one_stop_per_frame_in_the_anchor(
        // LLDB names it "::fillscope_frame_anchor(uint64_t)".
        find_stops(output, std::regex{R"(stop: SIGTRAP in \S*fillscope_frame_anchor\S* frame_index=(\d+) file=(\S+))"}),
        output);
    EXPECT_NE(output.find("exited: 0"), std::string::npos) << output;
    expect_all_frames_arrived();
}

}  // namespace
}  // namespace fillscope
