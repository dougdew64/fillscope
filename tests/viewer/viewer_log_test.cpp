// The viewer's log (viewer/viewer_log.h), its crash recording (viewer/crash_log.h), and what it
// records (viewer/log_events.h): the names, the lines, keeping the newest logs and nothing else
// touched, the size limit; a crash recorded by a process that really dies of it (GoogleTest's death
// tests run each in a process of its own); the feed's and answers' events, each said once, and the
// user's actions.
#include "support/answer_fixture.h"
#include "viewer/crash_log.h"
#include "viewer/log_events.h"
#include "viewer/viewer_log.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fillscope {
namespace {

using namespace std::chrono_literals;

// A directory of the test's own, removed however the test ends.
struct Scratch {
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::filesystem::remove_all(path); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       std::format("fillscope-log-{}-{}", ::getpid(), ::testing::UnitTest::GetInstance()->current_test_info()->name());
};

std::string read(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::set<std::string> names_in(const std::filesystem::path& directory) {
    std::set<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        names.insert(entry.path().filename().string());
    }
    return names;
}

// 2026-10-26 15:04:05 UTC.
const ViewerLog::WallClock::time_point kStarted = std::chrono::sys_days{std::chrono::year{2026} / 10 / 26} + 15h + 4min + 5s;

TEST(ViewerLog, IsNamedForWhenTheRunStartedAndItsProcess) {
    EXPECT_EQ(log_file_name(kStarted + 789ms, 4321), "viewer-20261026-150405-4321.log");
    EXPECT_TRUE(is_log_file_name("viewer-20261026-150405-4321.log"));
    for (const char* other : {"viewer-20261026-150405-.log", "viewer-20261026-150405-4321.log.sanitizer.4321",
                              "viewer-2026102-150405-1.log", "viewer-20261026150405-1.log", "context.json",
                              "viewer-20261026-15040x-1.log", "server-20261026-150405-1.log"}) {
        EXPECT_FALSE(is_log_file_name(other)) << other;
    }
}

TEST(ViewerLog, WritesEachLineAtOnceWithItsTimes) {
    const Scratch scratch;
    auto log = ViewerLog::open(scratch.path / "logs", kStarted, 4321);
    ASSERT_TRUE(log) << log.error();
    EXPECT_EQ(log->path(), scratch.path / "logs" / "viewer-20261026-150405-4321.log");
    EXPECT_TRUE(log->line("start: viewer"));
    // On disk at once, read through another descriptor: nothing waits in a buffer.
    EXPECT_TRUE(log->line("two lines\nin one"));
    const std::string text = read(log->path());
    const std::regex line{R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z \+\d+\.\d{3}s (.*)\n)"};
    std::vector<std::string> said;
    for (auto it = std::sregex_iterator{text.begin(), text.end(), line}; it != std::sregex_iterator{}; ++it) {
        said.push_back((*it)[1]);
    }
    EXPECT_EQ(said, (std::vector<std::string>{"start: viewer", "two lines / in one"}));
    EXPECT_EQ(log->bytes(), text.size());
}

TEST(ViewerLog, ARunsLogIsNeverAnothersNorAppendedTo) {
    const Scratch scratch;
    auto first = ViewerLog::open(scratch.path, kStarted, 4321);
    ASSERT_TRUE(first);
    const auto again = ViewerLog::open(scratch.path, kStarted, 4321);
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().find("can't create the log"), std::string::npos) << again.error();
}

TEST(ViewerLog, AStartingViewerKeepsTheNewestTwentyAndTouchesNothingElse) {
    const Scratch scratch;
    // 25 runs' logs, a minute apart; the oldest and the newest each with their sanitizers' report;
    // and files that aren't logs.
    std::vector<std::string> logs;
    for (int i = 0; i < 25; ++i) {
        logs.push_back(log_file_name(kStarted + std::chrono::minutes{i}, 100 + i));
        std::ofstream{scratch.path / logs.back()} << "a run";
    }
    std::ofstream{scratch.path / (logs.front() + ".sanitizer.100")} << "its report";
    std::ofstream{scratch.path / (logs.back() + ".sanitizer.124")} << "its report";
    std::ofstream{scratch.path / "notes.txt"} << "mine";
    std::filesystem::create_directory(scratch.path / "viewer-old");

    auto log = ViewerLog::open(scratch.path, kStarted + 1h, 999);
    ASSERT_TRUE(log) << log.error();
    std::set<std::string> expected{logs.begin() + 6, logs.end()};  // the newest 19 of the 25
    expected.insert(log->path().filename().string());               // and this run's: 20
    expected.insert(logs.back() + ".sanitizer.124");               // a kept log's report stays
    expected.insert("notes.txt");
    expected.insert("viewer-old");
    EXPECT_EQ(names_in(scratch.path), expected);
}

TEST(ViewerLog, StopsAtItsLimitSayingSo) {
    const Scratch scratch;
    auto log = ViewerLog::open(scratch.path, kStarted, 4321);
    ASSERT_TRUE(log);
    // Long lines, so filling 10 MB takes about a hundred writes, not thousands.
    const std::string long_line(100'000, 'x');
    int written = 0;
    while (log->line(long_line)) {
        ++written;
    }
    EXPECT_TRUE(log->full());
    EXPECT_GT(written, 100);
    EXPECT_FALSE(log->line("after"));  // nothing more
    const std::string text = read(log->path());
    const std::size_t limit_line = text.rfind('\n', text.size() - 2) + 1;
    // Every line the run wrote fits in the limit; the line saying it was reached comes after.
    EXPECT_LE(limit_line, ViewerLog::kMaxBytes);
    EXPECT_NE(text.find("the log has reached its limit, 10485760 bytes", limit_line), std::string::npos);
    EXPECT_EQ(text.find("after"), std::string::npos);
}

TEST(ViewerLog, SaysWhyItCantBeOpened) {
    const Scratch scratch;
    std::ofstream{scratch.path / "a-file"} << "not a directory";
    const auto log = ViewerLog::open(scratch.path / "a-file" / "logs", kStarted, 1);
    ASSERT_FALSE(log);
    EXPECT_NE(log.error().find("can't make the log directory"), std::string::npos) << log.error();
}

// ---- crash recording ----

// In the dying process: a log, with crashes recorded to it.
void record_crashes_to(const std::filesystem::path& file) {
    static auto log = ViewerLog::open(file.parent_path(), kStarted, ::getpid());  // must outlive the handlers
    if (!log) {
        std::abort();
    }
    std::filesystem::rename(log->path(), file);  // the descriptor stays the file's
    (void)log->line(install_crash_log(log->fd(), file.string() + ".sanitizer"));
}

// Each death test forks (GoogleTest's default style), so the dying child writes to the file the
// parent then reads: its paths are the parent's.
class CrashLog : public ::testing::Test {
protected:
    const Scratch scratch_;
    const std::filesystem::path file_ = scratch_.path / "crash.log";
};

TEST_F(CrashLog, AnAbortIsRecordedWithABacktrace) {
    EXPECT_EXIT(
        {
            record_crashes_to(file_);
            std::abort();
        },
        ::testing::KilledBySignal(SIGABRT), "");
    const std::string text = read(file_);
    EXPECT_NE(text.find("crash: fatal signal 6 (SIGABRT); the backtrace, innermost first:\n"), std::string::npos) << text;
    EXPECT_NE(text.find("crash: end of the backtrace"), std::string::npos) << text;
    EXPECT_NE(text.find("viewer_log_tests"), std::string::npos) << text;  // a frame of this program
}

TEST_F(CrashLog, AnUncaughtExceptionIsRecordedWithItsMessage) {
    EXPECT_EXIT(
        {
            record_crashes_to(file_);
            // Escaping a thread's function: std::terminate, as for any exception not caught.
            std::thread{[] { throw std::runtime_error{"the books went wrong"}; }}.join();
        },
        ::testing::KilledBySignal(SIGABRT), "");
    const std::string text = read(file_);
    EXPECT_NE(text.find("crash: std::terminate, an uncaught exception: the books went wrong; it aborts\n"), std::string::npos)
        << text;
    EXPECT_NE(text.find("crash: fatal signal 6 (SIGABRT)"), std::string::npos) << text;
}

#if !defined(__SANITIZE_ADDRESS__)
TEST_F(CrashLog, ASegmentationFaultIsRecordedAndStillEndsTheProcess) {
    EXPECT_EXIT(
        {
            record_crashes_to(file_);
            std::raise(SIGSEGV);
        },
        ::testing::KilledBySignal(SIGSEGV), "");
    const std::string text = read(file_);
    EXPECT_NE(text.find("crash: fatal signal 11 (SIGSEGV); the backtrace, innermost first:\n"), std::string::npos) << text;
}

TEST_F(CrashLog, AStackOverflowIsRecorded) {
    EXPECT_EXIT(
        {
            record_crashes_to(file_);
            struct Deep {
                // Recursion the compiler can't turn into a loop (each frame's array is used after the
                // call), bounded by a depth it can't see and the stack never reaches.
                static int down(int n) {
                    static volatile int depth = 1'000'000'000;
                    volatile char frame[4096];
                    frame[0] = static_cast<char>(n);
                    return n < depth ? down(n + 1) + frame[0] : 0;
                }
            };
            (void)Deep::down(0);
        },
        ::testing::KilledBySignal(SIGSEGV), "");
    EXPECT_NE(read(file_).find("crash: fatal signal 11 (SIGSEGV)"), std::string::npos);
}
#else
TEST_F(CrashLog, ASegmentationFaultIsLeftToAddressSanitizerWhichReportsBesideTheLog) {
    EXPECT_EXIT(
        {
            record_crashes_to(file_);
            std::raise(SIGSEGV);
        },
        ::testing::ExitedWithCode(1), "");
    std::string report;
    for (const auto& entry : std::filesystem::directory_iterator{scratch_.path}) {
        if (entry.path().filename().string().starts_with("crash.log.sanitizer.")) {
            report = read(entry.path());
        }
    }
    EXPECT_NE(report.find("AddressSanitizer: SEGV"), std::string::npos) << report;
    EXPECT_NE(read(file_).find("SIGSEGV, SIGBUS, SIGFPE left to AddressSanitizer"), std::string::npos);
}
#endif

TEST(CrashSignals, AreNamed) {
    EXPECT_EQ(signal_name(SIGSEGV), "SIGSEGV");
    EXPECT_EQ(signal_name(SIGABRT), "SIGABRT");
    EXPECT_EQ(signal_name(SIGUSR1), std::format("signal {}", SIGUSR1));
}

// ---- what is logged ----

MarketUpdate add(std::uint64_t seq, std::uint64_t id, std::int64_t price) {
    return {.seq = seq, .type = UpdateType::add, .order_id = id, .ticker = 0, .side = Side::buy, .price = price, .qty = 5, .priority = id};
}

TEST(LogEvents, TheFeedGoingLiveRecoveringAndRebuiltAreEachSaidOnce) {
    ViewerState state;
    LogWatch watch;
    EXPECT_TRUE(log_events(state, watch).empty());  // waiting, as the watch began
    state.books.on_incremental(add(1, 10, 100));
    EXPECT_EQ(log_events(state, watch), (std::vector<std::string>{"feed: live, the books as of update 1"}));
    state.books.on_incremental(add(2, 11, 99));
    EXPECT_TRUE(log_events(state, watch).empty());  // still live: nothing new to say
    state.books.on_incremental(add(4, 13, 98));     // update 3 never came
    const std::vector<std::string> recovering = log_events(state, watch);
    ASSERT_EQ(recovering.size(), 1u);
    EXPECT_TRUE(recovering[0].starts_with("feed: recovering, after update 2: ")) << recovering[0];
    ASSERT_TRUE(state.books.recovering_because());
    EXPECT_TRUE(recovering[0].ends_with(*state.books.recovering_because())) << recovering[0];
    // A snapshot as of update 4: the books rebuilt, then live.
    std::vector<MarketUpdate> snapshot{{.seq = 0, .type = UpdateType::snapshot_start, .order_id = 4}};
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        snapshot.push_back({.type = UpdateType::clear, .ticker = ticker});
    }
    snapshot.push_back({.type = UpdateType::snapshot_end, .order_id = 4});
    for (std::uint64_t i = 0; i < snapshot.size(); ++i) {
        snapshot[i].seq = i;
        state.books.on_snapshot(snapshot[i]);
    }
    const std::vector<std::string> rebuilt = log_events(state, watch);
    ASSERT_EQ(rebuilt.size(), 2u);
    EXPECT_TRUE(rebuilt[0].starts_with("feed: rebuilt from a snapshot as of update 4: the updates after 2 up to it were never "
                                       "applied one by one ("))
        << rebuilt[0];
    EXPECT_EQ(rebuilt[1], "feed: live, the books as of update 4");
    EXPECT_TRUE(log_events(state, watch).empty());
}

TEST(LogEvents, AnErrorReceivingIsSaidWhenItIsNew) {
    ViewerState state;
    LogWatch watch;
    state.feed_error = "a datagram of 7 bytes isn't whole updates";
    EXPECT_EQ(log_events(state, watch), (std::vector<std::string>{"feed: error receiving: a datagram of 7 bytes isn't whole updates"}));
    EXPECT_TRUE(log_events(state, watch).empty());
    state.feed_error = "another";
    EXPECT_EQ(log_events(state, watch), (std::vector<std::string>{"feed: error receiving: another"}));
}

TEST(LogEvents, AnAnswerShownOrRefusedIsSaidOnce) {
    const Scratch scratch;
    const std::filesystem::path file = scratch.path / "answer.json";
    test::write_maker_and_taker(file, "who trades?");
    ViewerState state;
    LogWatch watch;
    state.answer = AnswerShown{.file = file, .stamp = std::nullopt, .answer = std::nullopt, .problem = "there is no answer yet",
                               .frame = 0};
    EXPECT_EQ(log_events(state, watch), (std::vector<std::string>{std::format("answer: refused {}: there is no answer yet", file.string())}));
    EXPECT_TRUE(log_events(state, watch).empty());
    auto loaded = load_answer(file);
    ASSERT_TRUE(loaded) << loaded.error();
    const std::size_t steps = loaded->scenario.steps.size();
    const std::size_t frames = loaded->frames.size();
    state.answer->answer = std::move(*loaded);
    state.answer->stamp = stamp_of(file);
    state.answer->problem.reset();
    EXPECT_EQ(log_events(state, watch),
              (std::vector<std::string>{std::format("answer: showing {}: {} steps, {} frames", file.string(), steps, frames)}));
    EXPECT_TRUE(log_events(state, watch).empty());
}

TEST(LogEvents, TheUsersActionsAreSaidInOneLine) {
    EXPECT_EQ(describe(ViewerActions{}), std::nullopt);
    // What the user did, set on actions that are otherwise nothing.
    const auto said = [](auto set) {
        ViewerActions actions;
        set(actions);
        return describe(actions).value_or("nothing");
    };
    using A = ViewerActions&;
    EXPECT_EQ(said([](A a) { a.point_at = OrderRef{.ticker = 3, .order_id = 7}; }), "user: points at order 7 of ticker 3");
    EXPECT_EQ(said([](A a) { a.point_at = LevelRef{.ticker = 0, .side = Side::sell, .price = 101}; }),
              "user: points at the sell level at 101 of ticker 0");
    EXPECT_EQ(said([](A a) { a.follow = OrderRef{.ticker = 1, .order_id = 9}; }), "user: follows order 9 of ticker 1");
    EXPECT_EQ(said([](A a) { a.show_ticker = 6; }), "user: shows ticker 6");
    EXPECT_EQ(said([](A a) { a.replay_to = 40; }), "user: goes to update 40 of the replay");
    EXPECT_EQ(said([](A a) { a.answer_to = 2; }), "user: goes to frame 2 of the answer");
    EXPECT_EQ(said([](A a) { a.choose = 0; }), "user: chooses the latest question");
    EXPECT_EQ(said([](A a) { a.choose = 6; }), "user: chooses lesson 6");
    EXPECT_EQ(said([](A a) { a.clear_point = true; }), "user: stops pointing");
    EXPECT_EQ(said([](A a) { a.clear_follow = true; }), "user: stops following");
    EXPECT_EQ(said([](A a) {
                  a.point_at = OrderRef{.ticker = 0, .order_id = 1};
                  a.follow = OrderRef{.ticker = 0, .order_id = 1};
              }),
              "user: points at order 1 of ticker 0; follows order 1 of ticker 0");
}

}  // namespace
}  // namespace fillscope
