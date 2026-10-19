// The lockstep anchor under real debuggers, driven through the frame source (the program that calls
// it): every stop is in the anchor, in our own source file, with the right frame index, and
// Continue runs on to the next frame. Each test is skipped if its debugger isn't installed. Kept
// apart from lockstep_test.cpp because these are slow (the debugger tier).
//
// From the build: FILLSCOPE_FRAME_SOURCE and FILLSCOPE_FRAME_SOURCE_O2 (the frame source, as built
// and at -O2), FILLSCOPE_LLDB_DRIVER (lldb_driver.py) and FILLSCOPE_ANCHOR_SOURCE (anchor.cpp, to
// find the lines a stop may be at).

#include "channel/net.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fillscope {
namespace {

constexpr int kFrames = 3;

struct Stop {
    int frame_index;
    std::string file;               // the source file's name, without its directory
    int line;
    std::optional<int> received;    // messages the viewer had when the source stopped (LLDB driver)
};

// Every match of `pattern` in `output`. Groups: 1 the frame index, 2 the source path, 3 the line,
// and, if the pattern has it, 4 the messages received.
std::vector<Stop> find_stops(const std::string& output, const std::regex& pattern) {
    std::vector<Stop> stops;
    for (auto it = std::sregex_iterator(output.begin(), output.end(), pattern); it != std::sregex_iterator(); ++it) {
        const std::smatch& match = *it;
        Stop stop{.frame_index = std::stoi(match[1]),
                  .file = std::filesystem::path(match[2].str()).filename().string(),
                  .line = std::stoi(match[3]),
                  .received = std::nullopt};
        if (match.size() > 4 && match[4].matched) {
            stop.received = std::stoi(match[4]);
        }
        stops.push_back(stop);
    }
    return stops;
}

// The lines of anchor.cpp a stop can be at, found by what is on them rather than by number, so an
// edit above the anchor doesn't break the tests.
struct AnchorLines {
    int first_statement = 0;  // the anchor's first statement, where GDB puts a breakpoint by name
    int self_stop = 0;        // the breakpoint instruction, where a self-stop is reported
    int closing_brace = 0;    // the end of the anchor's body
};

AnchorLines anchor_lines() {
    std::ifstream source{FILLSCOPE_ANCHOR_SOURCE};
    AnchorLines lines;
    bool in_anchor = false;
    std::string text;
    for (int number = 1; std::getline(source, text); ++number) {
        if (text.starts_with("extern \"C\" [[gnu::noinline]] void fillscope_frame_anchor(")) {
            in_anchor = true;
        } else if (in_anchor && lines.first_statement == 0) {
            lines.first_statement = number;
        }
        if (in_anchor && text.find("asm volatile(\"int3") != std::string::npos) {
            lines.self_stop = number;
        }
        if (in_anchor && text == "}") {
            lines.closing_brace = number;
            break;
        }
    }
    return lines;
}

// One stop per frame, in order, each in the anchor's own file with its frame index readable, at a
// line `line_is_right` accepts.
template <typename LinePredicate>
void expect_one_stop_per_frame_in_the_anchor(const std::vector<Stop>& stops, const std::string& output,
                                             LinePredicate line_is_right) {
    ASSERT_EQ(stops.size(), static_cast<std::size_t>(kFrames)) << output;
    for (int i = 0; i < kFrames; ++i) {
        EXPECT_EQ(stops[i].frame_index, i) << output;
        EXPECT_EQ(stops[i].file, "anchor.cpp") << "the stop should open our own source\n" << output;
        EXPECT_TRUE(line_is_right(stops[i].line)) << "stopped at line " << stops[i].line << "\n" << output;
        if (stops[i].received) {
            // The lockstep claim: frame N is sent before the stop for frame N, so the viewer has
            // the run's description and frames 0 to N.
            EXPECT_EQ(*stops[i].received, i + 2) << "at the stop for frame " << i << "\n" << output;
        }
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

// The frame source at -O0 (the build's own) and at -O2 (built for these tests, to check that the
// anchor survives optimization: docs/DEBUGGER-LOCKSTEP.md, rule 1).
const std::string kSource = test::shell_quote(FILLSCOPE_FRAME_SOURCE);
const std::string kSourceO2 = test::shell_quote(FILLSCOPE_FRAME_SOURCE_O2);

// For the GDB tests, a viewer drained throughout (the frame source waits for its connection to be
// accepted); for the LLDB tests, the driver script is the viewer, on a socket of its own. What
// arrived is checked afterwards: the run's description and every frame.
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
    static std::string source_args(const std::string& socket) {
        return "--socket " + test::shell_quote(socket) + " --frames " + std::to_string(kFrames) + " --interval-ms 0";
    }

    void expect_all_frames_arrived() {
        // The frame source has exited, so everything it sent is waiting; a few drains take it all.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (receiver_.messages().size() < kFrames + 1u && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        EXPECT_EQ(receiver_.messages().size(), static_cast<std::size_t>(kFrames) + 1);  // the description, then the frames
    }

    // Runs `program` under the LLDB driver.
    std::string run_lldb_driver(const std::string& program, const std::string& arguments, bool by_name) const {
        return test::run_command("python3 -I " + test::shell_quote(FILLSCOPE_LLDB_DRIVER) +
                                 (by_name ? " --break-by-name " : " ") + test::shell_quote(driver_socket_) + " " +
                                 program + " " + arguments)
            .output;
    }

    const AnchorLines lines_ = anchor_lines();
    std::string name_ = test::unique_socket_name();
    test::BackgroundReceiver receiver_{name_};
    std::string driver_socket_ = test::unique_socket_name();
};

const std::regex kGdbBreakpointStop{R"(Breakpoint 1, fillscope_frame_anchor \(frame_index=(\d+)\) at (\S+):(\d+))"};
const std::regex kGdbSelfStop{
    R"(SIGTRAP[^\n]*\n(?:0x[0-9a-f]+ in )?fillscope_frame_anchor \(frame_index=(\d+)\) at (\S+):(\d+))"};
// LLDB names it "::fillscope_frame_anchor(uint64_t)". The driver names the actual signal, so a
// crash (SIGSEGV, SIGABRT) can't pass for a self-stop.
const std::regex kLldbSelfStop{
    R"(stop: signal SIGTRAP in \S*fillscope_frame_anchor\S* frame_index=(\d+) file=(\S+) line=(\d+) received=(\d+))"};
const std::regex kLldbBreakpointStop{
    R"(stop: breakpoint [^\n]* in \S*fillscope_frame_anchor\S* frame_index=(\d+) file=(\S+) line=(\d+) received=(\d+))"};

std::string gdb_by_name(const std::string& program, const std::string& arguments) {
    return test::run_command("gdb -batch -nx -ex 'break fillscope_frame_anchor' -ex " +
                             test::shell_quote("run " + arguments) + " -ex continue -ex continue -ex continue " + program)
        .output;
}

std::string gdb_self_stop(const std::string& program, const std::string& arguments) {
    return test::run_command("gdb -batch -nx -ex " + test::shell_quote("run --self-stop " + arguments) +
                             " -ex continue -ex continue -ex continue " + program)
        .output;
}

TEST_F(LockstepDebugger, GdbBreakpointByNameStopsAtTheAnchorsFirstStatement) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = gdb_by_name(kSource, source_args(name_));
    expect_one_stop_per_frame_in_the_anchor(find_stops(output, kGdbBreakpointStop), output,
                                            [&](int line) { return line == lines_.first_statement; });
    EXPECT_NE(output.find("exited normally"), std::string::npos) << output;
    expect_all_frames_arrived();
}

TEST_F(LockstepDebugger, GdbSelfStopStopsAtTheBreakpointInstruction) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = gdb_self_stop(kSource, source_args(name_));
    expect_one_stop_per_frame_in_the_anchor(find_stops(output, kGdbSelfStop), output,
                                            [&](int line) { return line == lines_.self_stop; });
    EXPECT_NE(output.find("exited normally"), std::string::npos) << output;
    expect_all_frames_arrived();
}

// LLDB places a breakpoint by name after the anchor's stores, at the `if`: inside the body, not
// necessarily at its first statement.
TEST_F(LockstepDebugger, LldbBreakpointByNameStopsInTheAnchorAfterEachFrameIsSent) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = run_lldb_driver(kSource, source_args(driver_socket_), true);
    expect_one_stop_per_frame_in_the_anchor(find_stops(output, kLldbBreakpointStop), output, [&](int line) {
        return line >= lines_.first_statement && line < lines_.closing_brace;
    });
    EXPECT_NE(output.find("exited: 0"), std::string::npos) << output;
}

TEST_F(LockstepDebugger, LldbSelfStopStopsAtTheBreakpointInstructionAfterEachFrameIsSent) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = run_lldb_driver(kSource, "--self-stop " + source_args(driver_socket_), false);
    expect_one_stop_per_frame_in_the_anchor(find_stops(output, kLldbSelfStop), output,
                                            [&](int line) { return line == lines_.self_stop; });
    EXPECT_NE(output.find("exited: 0"), std::string::npos) << output;
}

// The same, with the frame source and the anchor built at -O2: the anchor is still a function of
// its own, both debuggers stop in it, and frame_index is still readable.
TEST_F(LockstepDebugger, AtO2GdbBreakpointByNameStopsInTheAnchor) {
    REQUIRE_DEBUGGER("gdb");
    std::string output = gdb_by_name(kSourceO2, source_args(name_));
    expect_one_stop_per_frame_in_the_anchor(find_stops(output, kGdbBreakpointStop), output, [&](int line) {
        return line >= lines_.first_statement && line < lines_.closing_brace;
    });
    expect_all_frames_arrived();
}

TEST_F(LockstepDebugger, AtO2GdbSelfStopReadsTheFrameIndex) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = gdb_self_stop(kSourceO2, source_args(name_));
    expect_one_stop_per_frame_in_the_anchor(find_stops(output, kGdbSelfStop), output,
                                            [&](int line) { return line == lines_.self_stop; });
    expect_all_frames_arrived();
}

TEST_F(LockstepDebugger, AtO2LldbSelfStopReadsTheFrameIndex) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = run_lldb_driver(kSourceO2, "--self-stop " + source_args(driver_socket_), false);
    expect_one_stop_per_frame_in_the_anchor(find_stops(output, kLldbSelfStop), output,
                                            [&](int line) { return line == lines_.self_stop; });
}

// The driver names the signal a program stopped with, so the self-stop tests can't mistake a crash
// for a self-stop.
TEST_F(LockstepDebugger, LldbDriverNamesTheSignalOfACrash) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = run_lldb_driver("/bin/sh", "-c 'kill -SEGV $$'", false);
    EXPECT_NE(output.find("stop: signal SIGSEGV"), std::string::npos) << output;
    EXPECT_EQ(output.find("SIGTRAP"), std::string::npos) << output;
}

}  // namespace
}  // namespace fillscope
