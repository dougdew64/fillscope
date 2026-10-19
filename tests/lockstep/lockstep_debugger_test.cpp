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

// Skips the test when `debugger` isn't installed, or fails it when FILLSCOPE_REQUIRE_DEBUGGERS is
// set (as CI sets it, so a missing debugger can't pass as a skipped test). Wrapped in
// do { ... } while (false) so the macro is one statement: used after an `if` without braces, a bare
// `if` inside would capture a following `else`.
#define REQUIRE_DEBUGGER(debugger)                                              \
    do {                                                                        \
        if (!test::command_exists(debugger)) {                                  \
            if (std::getenv("FILLSCOPE_REQUIRE_DEBUGGERS") != nullptr) {        \
                FAIL() << debugger " not installed, and this run requires it"; \
            }                                                                   \
            GTEST_SKIP() << debugger " not installed";                          \
        }                                                                       \
    } while (false)

// A listener for the frame source to send to, and the frame source's arguments pointing at it. The
// messages are drained afterwards, to check the run's description and every frame arrived.
class LockstepDebugger : public ::testing::Test {
protected:
    void SetUp() override {
        // LeakSanitizer can't run under ptrace; without this the sanitizer build's frame source
        // fails at exit. Added to any options already set, rather than replacing them.
        const char* existing = std::getenv("ASAN_OPTIONS");
        const std::string options = existing == nullptr || *existing == '\0' ? std::string{"detect_leaks=0"}
                                                                              : std::string{existing} + ":detect_leaks=0";
        ::setenv("ASAN_OPTIONS", options.c_str(), 1);
    }

    // The frame source's arguments, ready for a shell command line.
    std::string source_args() const {
        return "--socket " + test::shell_quote(name_) + " --frames " + std::to_string(kFrames) + " --interval-ms 0";
    }

    void expect_all_frames_arrived() {
        std::vector<Message> messages;
        receiver_.drain(messages);
        EXPECT_EQ(messages.size(), static_cast<std::size_t>(kFrames) + 1);  // the description, then the frames
    }

    const std::string source_ = test::shell_quote(FILLSCOPE_FRAME_SOURCE);
    std::string name_ = test::unique_socket_name();
    MessageReceiver receiver_ = test::listen(name_);
};

TEST_F(LockstepDebugger, GdbBreakpointByNameStopsInTheAnchor) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = test::run_command("gdb -batch -nx -ex 'break fillscope_frame_anchor' -ex " +
                                                 test::shell_quote("run " + source_args()) +
                                                 " -ex continue -ex continue -ex continue " + source_)
                                   .output;
    expect_one_stop_per_frame_in_the_anchor(
        find_stops(output, std::regex{R"(Breakpoint 1, fillscope_frame_anchor \(frame_index=(\d+)\) at (\S+):\d+)"}),
        output);
    EXPECT_NE(output.find("exited normally"), std::string::npos) << output;
    expect_all_frames_arrived();
}

TEST_F(LockstepDebugger, GdbSelfStopStopsInTheAnchor) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = test::run_command("gdb -batch -nx -ex " + test::shell_quote("run --self-stop " + source_args()) +
                                                 " -ex continue -ex continue -ex continue " + source_)
                                   .output;
    expect_one_stop_per_frame_in_the_anchor(
        find_stops(output, std::regex{R"(SIGTRAP[^\n]*\n(?:0x[0-9a-f]+ in )?fillscope_frame_anchor \(frame_index=(\d+)\) at (\S+):\d+)"}),
        output);
    EXPECT_NE(output.find("exited normally"), std::string::npos) << output;
    expect_all_frames_arrived();
}

TEST_F(LockstepDebugger, LldbBreakpointByNameStopsInTheAnchor) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = test::run_command("lldb --batch -o 'breakpoint set -n fillscope_frame_anchor' -o " +
                                                 test::shell_quote("run " + source_args()) +
                                                 " -o continue -o continue -o continue " + source_)
                                   .output;
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
    const std::string output = test::run_command("python3 -I " + test::shell_quote(FILLSCOPE_LLDB_SCRIPT) + " " +
                                                 source_ + " --self-stop " + source_args())
                                   .output;
    expect_one_stop_per_frame_in_the_anchor(
        // LLDB names it "::fillscope_frame_anchor(uint64_t)". The script names the actual signal, so
        // a crash (SIGSEGV, SIGABRT) can't pass for a self-stop.
        find_stops(output, std::regex{R"(stop: signal SIGTRAP in \S*fillscope_frame_anchor\S* frame_index=(\d+) file=(\S+))"}),
        output);
    EXPECT_NE(output.find("exited: 0"), std::string::npos) << output;
    expect_all_frames_arrived();
}

}  // namespace
}  // namespace fillscope
