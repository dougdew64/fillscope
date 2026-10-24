// Debugger lockstep with the matcher, on a replay (docs/DEBUGGER-LOCKSTEP.md): the back end's
// exchange_replay, publishing each request's market updates and then calling the lockstep anchor
// (backend/Chapter12/exchange/matcher/lockstep_anchor.h), under GDB and LLDB, by breakpoint and by
// self-stop, at -O0 and -O2. Every stop is in the anchor, in its own source file, with the request's
// number and the updates published so far readable; under LLDB the driver is the viewer, and at the
// stop after request N it has every update the replay published through N. Each test is skipped if
// its debugger isn't installed (and fails, if FILLSCOPE_REQUIRE_DEBUGGERS is set).
//
// From the build: FILLSCOPE_EXCHANGE_REPLAY and FILLSCOPE_EXCHANGE_REPLAY_O2 (as built, and at -O2),
// FILLSCOPE_REPLAY_LLDB_DRIVER (replay_lldb_driver.py) and FILLSCOPE_REQUEST_ANCHOR_SOURCE (the
// anchor's source, to find the lines a stop may be at).

#include "support/test_support.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fillscope {
namespace {

// The recording: three requests on ticker 1. A sell of 5 at 100 rests (1 update: its add); a buy of
// 2 at 100 trades with it (2: the trade, the resting order's modify); the sell is cancelled (1).
constexpr std::uint64_t kPublishedAfter[] = {1, 3, 4};
constexpr int kRequests = 3;

// One MEClientRequest, as the recording holds it (request_recording.h there): packed, little-endian.
std::string request(std::uint8_t type, std::uint32_t client, std::uint64_t order, std::int8_t side, std::int64_t price,
                    std::uint32_t qty) {
    std::string bytes;
    const auto put = [&](const auto& value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof value); };
    put(type);
    put(client);
    put(std::uint32_t{1});  // the ticker
    put(order);
    put(side);
    put(price);
    put(qty);
    return bytes;
}

struct Stop {
    int request_number;
    int published;
    std::string file;
    int line;
    std::optional<int> received;  // updates the viewer had when the replay stopped (LLDB driver)
};

// Every match of `pattern` in `output`. Groups: 1 the request's number, 2 the updates published, 3
// the source path, 4 the line, and, if the pattern has it, 5 the updates received.
std::vector<Stop> find_stops(const std::string& output, const std::regex& pattern) {
    std::vector<Stop> stops;
    for (auto it = std::sregex_iterator(output.begin(), output.end(), pattern); it != std::sregex_iterator(); ++it) {
        const std::smatch& match = *it;
        Stop stop{.request_number = std::stoi(match[1]),
                  .published = std::stoi(match[2]),
                  .file = std::filesystem::path(match[3].str()).filename().string(),
                  .line = std::stoi(match[4]),
                  .received = std::nullopt};
        if (match.size() > 5 && match[5].matched) {
            stop.received = std::stoi(match[5]);
        }
        stops.push_back(stop);
    }
    return stops;
}

// The anchor's lines a stop can be at, found by what is on them rather than by number.
struct AnchorLines {
    int first_statement = 0;  // where GDB puts a breakpoint by name
    int self_stop = 0;        // the breakpoint instruction, where a self-stop is reported
    int closing_brace = 0;
};

AnchorLines anchor_lines() {
    std::ifstream source{FILLSCOPE_REQUEST_ANCHOR_SOURCE};
    AnchorLines lines;
    bool in_anchor = false;
    std::string text;
    for (int number = 1; std::getline(source, text); ++number) {
        if (text.starts_with("extern \"C\" [[gnu::noinline]] void fillscope_request_anchor(")) {
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

// One stop per request, in order, each in the anchor's file with its arguments readable, at a line
// `line_is_right` accepts.
template <typename LinePredicate>
void expect_one_stop_per_request_in_the_anchor(const std::vector<Stop>& stops, const std::string& output,
                                               LinePredicate line_is_right) {
    ASSERT_EQ(stops.size(), static_cast<std::size_t>(kRequests)) << output;
    for (int i = 0; i < kRequests; ++i) {
        EXPECT_EQ(stops[i].request_number, i + 1) << output;
        EXPECT_EQ(stops[i].published, static_cast<int>(kPublishedAfter[i])) << output;
        EXPECT_EQ(stops[i].file, "lockstep_anchor.cpp") << "the stop should open the anchor's own source\n" << output;
        EXPECT_TRUE(line_is_right(stops[i].line)) << "stopped at line " << stops[i].line << "\n" << output;
        if (stops[i].received) {
            // The lockstep claim: request N's updates are sent before its stop, so the viewer has
            // every update published through request N.
            EXPECT_EQ(*stops[i].received, stops[i].published) << "at the stop after request " << i + 1 << "\n" << output;
        }
    }
}

#define REQUIRE_DEBUGGER(debugger)                                              \
    do {                                                                        \
        if (!test::command_exists(debugger)) {                                  \
            if (std::getenv("FILLSCOPE_REQUIRE_DEBUGGERS") != nullptr) {        \
                FAIL() << debugger " not installed, and this run requires it"; \
            }                                                                   \
            GTEST_SKIP() << debugger " not installed";                          \
        }                                                                       \
    } while (false)

// Counts the updates published to a group, on a thread of its own, for the GDB tests: what arrived
// is checked once the replay has exited.
class Listener {
public:
    Listener(const std::string& group, std::uint16_t port) : fd_{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)} {
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {}, .sin_zero = {}};
        ::inet_pton(AF_INET, group.c_str(), &address.sin_addr);
        EXPECT_EQ(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0) << std::strerror(errno);
        ip_mreq membership{.imr_multiaddr = address.sin_addr, .imr_interface = {htonl(INADDR_ANY)}};
        EXPECT_EQ(::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof membership), 0);
        const timeval timeout{0, 50'000};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        thread_ = std::jthread{[this](std::stop_token stop) {
            std::vector<char> buffer(65536);
            while (!stop.stop_requested()) {
                if (const auto n = ::recv(fd_, buffer.data(), buffer.size(), 0); n > 0) {
                    updates_ += static_cast<int>(n / 42);
                }
            }
        }};
    }
    ~Listener() {
        thread_.request_stop();
        thread_.join();
        ::close(fd_);
    }
    // The updates arrived, once at least `expected` have or a second has passed.
    int updates(int expected) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (updates_ < expected && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return updates_;
    }

private:
    int fd_;
    std::atomic<int> updates_{0};
    std::jthread thread_;
};

// What the replay prints as it finishes: it ran to its end after the stops. (Checked rather than
// the debugger's own report of the exit, which GDB sometimes loses, "Couldn't get registers: No such
// process", as the replay exits with its logger threads still running.)
constexpr std::string_view kFinished = "exchange_replay: 3 requests, 4 market updates";

const std::string kReplay = test::shell_quote(FILLSCOPE_EXCHANGE_REPLAY);
const std::string kReplayO2 = test::shell_quote(FILLSCOPE_EXCHANGE_REPLAY_O2);

class ReplayLockstep : public ::testing::Test {
protected:
    void SetUp() override {
        // LeakSanitizer can't run under ptrace; added to any options already set.
        const char* existing = std::getenv("ASAN_OPTIONS");
        const std::string options = existing == nullptr || *existing == '\0' ? std::string{"detect_leaks=0"}
                                                                              : std::string{existing} + ":detect_leaks=0";
        ::setenv("ASAN_OPTIONS", options.c_str(), 1);
        dir_ = std::filesystem::temp_directory_path() /
               std::format("fillscope-replay-{}-{}", ::getpid(), ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(dir_);
        std::ofstream{dir_ / "three.rec", std::ios::binary}
            << "LLREQ001" << request(1, 1, 10, -1, 100, 5) << request(1, 2, 20, 1, 100, 2) << request(2, 1, 10, 0, 0, 0);
        std::filesystem::current_path(dir_);  // the replay writes its logs in the working directory
    }
    void TearDown() override {
        std::filesystem::current_path(std::filesystem::temp_directory_path());
        std::filesystem::remove_all(dir_);
    }

    // A group and port of the test's own, from its name: tests run in parallel, each in a process of
    // its own. A socket is bound to its group's address, so tests on different groups never meet,
    // even on one port (AStreamOfItsOwn checks none share both).
    static std::size_t stream_of(const std::string& test) { return std::hash<std::string>{}(test) % (200 * 800); }
    std::size_t stream() const { return stream_of(::testing::UnitTest::GetInstance()->current_test_info()->name()); }
    std::uint16_t port() const { return static_cast<std::uint16_t>(24100 + stream() / 200); }
    std::string group() const { return std::format("233.252.18.{}", 1 + stream() % 200); }
    std::string publish_args() const { return std::format("three.rec --publish {}:{}", group(), port()); }

    std::string gdb_by_name(const std::string& program) const {
        return test::run_command("gdb -batch -nx -ex 'break fillscope_request_anchor' -ex " +
                                 test::shell_quote("run " + publish_args()) + " -ex continue -ex continue -ex continue " +
                                 program)
            .output;
    }
    std::string gdb_self_stop(const std::string& program) const {
        return test::run_command("gdb -batch -nx -ex " + test::shell_quote("run " + publish_args() + " --self-stop") +
                                 " -ex continue -ex continue -ex continue " + program)
            .output;
    }
    std::string lldb_driver(const std::string& program, bool by_name, const std::string& extra = "") const {
        return test::run_command(std::format("python3 -I {}{} {} {} {} {}{}", test::shell_quote(FILLSCOPE_REPLAY_LLDB_DRIVER),
                                             by_name ? " --break-by-name" : "", group(), port(), program, publish_args(),
                                             extra))
            .output;
    }

    const AnchorLines lines_ = anchor_lines();
    std::filesystem::path dir_;
};

// GDB reports a stop of a program with threads as "Thread 1 "name" hit Breakpoint 1, ...".
const std::regex kGdbBreakpointStop{
    R"(Breakpoint 1, fillscope_request_anchor \(request_number=(\d+), updates_published=(\d+)\) at (\S+):(\d+))"};
const std::regex kGdbSelfStop{
    R"(SIGTRAP[^\n]*\n(?:0x[0-9a-f]+ in )?fillscope_request_anchor \(request_number=(\d+), updates_published=(\d+)\) at (\S+):(\d+))"};
const std::regex kLldbSelfStop{
    R"(stop: signal SIGTRAP in \S*fillscope_request_anchor[^\n]* request_number=(\d+) updates_published=(\d+) file=(\S+) line=(\d+) received=(\d+))"};
const std::regex kLldbBreakpointStop{
    R"(stop: breakpoint [^\n]* in \S*fillscope_request_anchor[^\n]* request_number=(\d+) updates_published=(\d+) file=(\S+) line=(\d+) received=(\d+))"};

TEST_F(ReplayLockstep, GdbBreakpointByNameStopsAtTheAnchorsFirstStatement) {
    REQUIRE_DEBUGGER("gdb");
    const Listener viewer{group(), port()};
    const std::string output = gdb_by_name(kReplay);
    expect_one_stop_per_request_in_the_anchor(find_stops(output, kGdbBreakpointStop), output,
                                              [&](int line) { return line == lines_.first_statement; });
    EXPECT_NE(output.find(kFinished), std::string::npos) << output;
    EXPECT_EQ(viewer.updates(4), 4);
}

TEST_F(ReplayLockstep, GdbSelfStopStopsAtTheBreakpointInstruction) {
    REQUIRE_DEBUGGER("gdb");
    const Listener viewer{group(), port()};
    const std::string output = gdb_self_stop(kReplay);
    expect_one_stop_per_request_in_the_anchor(find_stops(output, kGdbSelfStop), output,
                                              [&](int line) { return line == lines_.self_stop; });
    EXPECT_NE(output.find(kFinished), std::string::npos) << output;
    EXPECT_EQ(viewer.updates(4), 4);
}

// LLDB places a breakpoint by name after the anchor's store, at the `if`.
TEST_F(ReplayLockstep, LldbBreakpointByNameStopsInTheAnchorWithEveryUpdateArrived) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = lldb_driver(kReplay, true);
    expect_one_stop_per_request_in_the_anchor(find_stops(output, kLldbBreakpointStop), output, [&](int line) {
        return line >= lines_.first_statement && line < lines_.closing_brace;
    });
    EXPECT_NE(output.find(kFinished), std::string::npos) << output;
}

TEST_F(ReplayLockstep, LldbSelfStopStopsAtTheBreakpointInstructionWithEveryUpdateArrived) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = lldb_driver(kReplay, false, " --self-stop");
    expect_one_stop_per_request_in_the_anchor(find_stops(output, kLldbSelfStop), output,
                                              [&](int line) { return line == lines_.self_stop; });
    EXPECT_NE(output.find(kFinished), std::string::npos) << output;
}

// The same with the replay and the anchor built at -O2: the anchor is still a function of its own,
// both debuggers stop in it, and its arguments are still readable.
TEST_F(ReplayLockstep, AtO2GdbBreakpointByNameStopsInTheAnchor) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = gdb_by_name(kReplayO2);
    expect_one_stop_per_request_in_the_anchor(find_stops(output, kGdbBreakpointStop), output, [&](int line) {
        return line >= lines_.first_statement && line < lines_.closing_brace;
    });
}

TEST_F(ReplayLockstep, AtO2GdbSelfStopReadsTheArguments) {
    REQUIRE_DEBUGGER("gdb");
    const std::string output = gdb_self_stop(kReplayO2);
    expect_one_stop_per_request_in_the_anchor(find_stops(output, kGdbSelfStop), output,
                                              [&](int line) { return line == lines_.self_stop; });
}

TEST_F(ReplayLockstep, AtO2LldbSelfStopReadsTheArguments) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = lldb_driver(kReplayO2, false, " --self-stop");
    expect_one_stop_per_request_in_the_anchor(find_stops(output, kLldbSelfStop), output,
                                              [&](int line) { return line == lines_.self_stop; });
}

// Each test's stream (group and port) is its own.
TEST_F(ReplayLockstep, AStreamOfItsOwn) {
    std::set<std::size_t> streams;
    const ::testing::TestSuite* suite = ::testing::UnitTest::GetInstance()->current_test_suite();
    for (int i = 0; i < suite->total_test_count(); ++i) {
        EXPECT_TRUE(streams.insert(stream_of(suite->GetTestInfo(i)->name())).second) << suite->GetTestInfo(i)->name();
    }
}

// The replay's command line is read exactly: each of these was once accepted as something else,
// and each is refused with the usage, while the same line with good values runs to its end.
TEST_F(ReplayLockstep, ABadCommandLineIsRefused) {
    const std::string stream = std::format("{}:{}", group(), port());
    const std::vector<std::string> bad_lines = {
        std::format("--publish {} --interval-ms ''", stream),
        std::format("--publish {} --interval-ms 99999999999999999999", stream),
        std::format("--publish {} --interval-ms -5", stream),
        std::format("--publish {}:' 99'", group()),
        std::format("--publish {}:+99", group()),
        std::format("--publish {}:0", group()),
        "--publish not.an.address:24000",
        "--publish 233.252.18.1"};
    for (const std::string& bad : bad_lines) {
        const auto result = test::run_command(kReplay + " three.rec " + bad);
        EXPECT_EQ(result.status, 2) << bad << "\n" << result.output;
        EXPECT_NE(result.output.find("usage: exchange_replay"), std::string::npos) << bad << "\n" << result.output;
    }
    const auto good = test::run_command(kReplay + " three.rec --publish " + stream + " --interval-ms 0");
    EXPECT_EQ(good.status, 0) << good.output;
    EXPECT_NE(good.output.find(kFinished), std::string::npos) << good.output;
}

// Without --self-stop or a breakpoint, a replay under a debugger runs straight through.
TEST_F(ReplayLockstep, WithoutSelfStopNoStop) {
    REQUIRE_DEBUGGER("lldb");
    const std::string output = lldb_driver(kReplay, false);
    EXPECT_EQ(output.find("stop:"), std::string::npos) << output;
    EXPECT_NE(output.find(kFinished), std::string::npos) << output;
}

}  // namespace
}  // namespace fillscope
