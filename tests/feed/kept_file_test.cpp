// The file the viewer writes what it keeps to (feed/kept_file.h): read back, it is what was kept,
// through a feed with gaps and recoveries; read while it is still being written, cut anywhere, it is
// what was written up to its last whole record; a file that isn't one is refused, saying why.
#include "feed/books_text.h"
#include "feed/kept_file.h"
#include "simulated_feed.h"

#include <gtest/gtest.h>

#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fillscope {
namespace {

using namespace fillscope::test;

// A directory of the test's own, removed however the test ends.
struct Scratch {
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::filesystem::remove_all(path); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       std::format("fillscope-kept-{}-{}", ::getpid(), ::testing::UnitTest::GetInstance()->current_test_info()->name());
};

std::string bytes_of(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The feed with faults, written to `file` as the viewer writes it: after each update is kept.
void write_while_feeding(Viewer& viewer, const std::filesystem::path& file) {
    viewer.check = [](std::uint64_t) { return false; };  // the buffer itself is tested elsewhere
    auto writer = KeptFileWriter::create(file);
    ASSERT_TRUE(writer) << writer.error();
    viewer.on_kept = [&](const TimeShiftBuffer& kept) { ASSERT_TRUE(writer->write_new(kept)); };
    feed_with_faults(viewer, 1500);  // each fault once; reading a file afresh, many times, costs
}

TEST(KeptFile, ReadBackItIsWhatWasKept) {
    const Scratch scratch;
    Viewer viewer;
    write_while_feeding(viewer, scratch.path / "kept");
    const auto read = read_kept_file(scratch.path / "kept");
    ASSERT_TRUE(read) << read.error();
    const TimeShiftBuffer& kept = viewer.kept;
    ASSERT_GE(kept.runs().size(), 3u);  // the late join, the loss, the swap
    ASSERT_EQ(read->runs(), kept.runs());
    EXPECT_EQ(read->kept(), kept.kept());
    for (const TimeShiftBuffer::Run& run : kept.runs()) {
        // Every update's arrival; the books at each run's start and end, and one update in 17.
        for (std::uint64_t seq = run.first; seq <= run.last; ++seq) {
            EXPECT_EQ(read->arrived(seq), kept.arrived(seq)) << "update " << seq;
            if (seq == run.first || seq == run.last || seq % 17 == 0) {
                expect_same(read->moment(seq).value(), kept.moment(seq).value());
            }
        }
    }
}

// A run's start is its books, written whole: its resting orders and each ticker's last trade, which
// a snapshot's runs (the feed's) never have, since a snapshot clears them; a run that starts after
// the updates queued behind a snapshot can.
TEST(KeptFile, ARunsStartKeepsItsOrdersAndLastTrades) {
    const Scratch scratch;
    Moment base;
    base.seq = 10;
    ASSERT_TRUE(base.books[3].apply({.seq = 7, .type = UpdateType::add, .order_id = 5, .ticker = 3, .side = Side::sell, .price = 101,
                                     .qty = 4, .priority = 1}));
    base.last_trades[3] = MarketUpdate{.seq = 9, .type = UpdateType::trade, .order_id = 0, .ticker = 3, .side = Side::buy, .price = 101,
                                       .qty = 2, .priority = 0};
    base.last_trades[6] = MarketUpdate{.seq = 8, .type = UpdateType::trade, .order_id = 0, .ticker = 6, .side = Side::sell, .price = 99,
                                       .qty = 1, .priority = 0};
    TimeShiftBuffer kept;
    ASSERT_TRUE(kept.start_run(base));
    ASSERT_TRUE(kept.append({.seq = 11, .type = UpdateType::add, .order_id = 6, .ticker = 3, .side = Side::sell, .price = 102, .qty = 1,
                             .priority = 2},
                            TimeShiftBuffer::Clock::time_point{std::chrono::seconds{5}}));
    auto writer = KeptFileWriter::create(scratch.path / "kept");
    ASSERT_TRUE(writer);
    ASSERT_TRUE(writer->write_new(kept));
    const auto read = read_kept_file(scratch.path / "kept");
    ASSERT_TRUE(read) << read.error();
    expect_same(read->moment(10).value(), base);
    EXPECT_EQ(read->moment(11).value().books[3].order_count(), 2u);
    EXPECT_EQ(read->arrived(11), TimeShiftBuffer::Clock::time_point{std::chrono::seconds{5}});
}

// It says how far it has got as it goes, with the time since it began: on CI it has taken more
// than its 2 s in the sanitizer build, where it takes about 0.3 s, and a test stopped for its time
// prints only what it wrote, so the next time says where the time went (CTest shows a test's
// output only when it fails).
TEST(KeptFile, ReadWhileBeingWrittenItIsWhatWasWrittenToTheLastWholeRecord) {
    const auto began = std::chrono::steady_clock::now();
    const auto progress = [&](const std::string& what) {
        std::printf("%5.0f ms: %s\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(),
                    what.c_str());
        std::fflush(stdout);
    };
    const Scratch scratch;
    Viewer viewer;
    write_while_feeding(viewer, scratch.path / "kept");
    const std::string whole = bytes_of(scratch.path / "kept");
    progress(std::format("written and read back whole: {} bytes, in {}", whole.size(), scratch.path.string()));
    // Cut at every byte of the header, at every 293rd byte inside the first run's start record, at every
    // byte from its last 16 through the update after it, at each edge (and either side) of the next
    // updates, and at 3 places spread over the rest: each is a file a reader could find while the
    // viewer writes it. (Each read builds what the file holds afresh, so not at every byte of the
    // whole file.)
    ASSERT_EQ(whole[kKeptFileMagic.size()], 'R');  // the feed joins late: it starts with a snapshot's books
    std::uint32_t orders = 0;
    std::memcpy(&orders, whole.data() + kKeptFileMagic.size() + 1 + 8, sizeof orders);
    const std::size_t orders_end = kKeptFileMagic.size() + 1 + 8 + 4 + std::size_t{orders} * kUpdateSize;
    const auto trades = static_cast<std::uint8_t>(whole[orders_end]);
    const std::size_t run_end = orders_end + 1 + std::size_t{trades} * kUpdateSize;
    constexpr std::size_t kUpdateRecord = 1 + kUpdateSize + 8;
    std::vector<std::size_t> cuts;
    for (std::size_t length = 0; length <= kKeptFileMagic.size() + 13; ++length) {
        cuts.push_back(length);
    }
    for (std::size_t length = kKeptFileMagic.size() + 13; length < run_end - 16; length += 293) {
        cuts.push_back(length);
    }
    for (std::size_t length = run_end - 16; length <= run_end + kUpdateRecord; ++length) {
        cuts.push_back(length);
    }
    for (std::size_t record = 2; record <= 10; ++record) {  // the next updates: each edge, and either side
        for (const std::size_t length : {run_end + record * kUpdateRecord - 1, run_end + record * kUpdateRecord,
                                         run_end + record * kUpdateRecord + 1}) {
            cuts.push_back(length);
        }
    }
    for (std::size_t i = 1; i <= 3; ++i) {
        cuts.push_back(whole.size() * i / 4);
    }
    std::size_t read_some = 0;
    for (const std::size_t length : cuts) {
        std::ofstream{scratch.path / "cut", std::ios::binary | std::ios::trunc} << whole.substr(0, length);
        progress(std::format("cut at {} written", length));
        const auto read = read_kept_file(scratch.path / "cut");
        progress("read");
        if (length < kKeptFileMagic.size()) {
            EXPECT_FALSE(read) << length;  // not even the header yet
            continue;
        }
        ASSERT_TRUE(read) << "cut at " << length << ": " << read.error();
        // A prefix of what was kept: the same runs, the last perhaps shorter, and the same books.
        const auto& runs = read->runs();
        ASSERT_LE(runs.size(), viewer.kept.runs().size());
        for (std::size_t i = 0; i < runs.size(); ++i) {
            EXPECT_EQ(runs[i].first, viewer.kept.runs()[i].first);
            EXPECT_LE(runs[i].last, viewer.kept.runs()[i].last);
        }
        if (!runs.empty()) {
            expect_same(read->moment(runs.back().last).value(), viewer.kept.moment(runs.back().last).value());
            ++read_some;
        }
    }
    EXPECT_GT(read_some, kUpdateRecord);  // those from the first run's start on
    progress(std::format("done: {} cuts", cuts.size()));
}

TEST(KeptFile, AFileThatIsntOneIsRefusedSayingWhy) {
    const Scratch scratch;
    EXPECT_NE(read_kept_file(scratch.path / "missing").error().find("can't open"), std::string::npos);
    std::ofstream{scratch.path / "other"} << "LLMDAT01 a market data file";
    EXPECT_NE(read_kept_file(scratch.path / "other").error().find("isn't a kept file (it doesn't begin with FSKEEP01)"), std::string::npos);
    std::ofstream{scratch.path / "bad"} << "FSKEEP01X";
    EXPECT_NE(read_kept_file(scratch.path / "bad").error().find("byte 8 begins no record"), std::string::npos);
    const auto writer = KeptFileWriter::create(scratch.path / "no-such-directory" / "kept");
    ASSERT_FALSE(writer);
    EXPECT_NE(writer.error().find("can't create"), std::string::npos);
}

// In a process of its own: writes a feed to `file` with the largest file the process may write set
// small, and exits 0 if a write then failed saying why.
[[noreturn]] void write_past_the_limit(const std::filesystem::path& file) {
    std::signal(SIGXFSZ, SIG_IGN);  // a write past the limit then fails, rather than ending the process
    rlimit limit{};
    limit.rlim_cur = 4096;
    limit.rlim_max = RLIM_INFINITY;
    ::setrlimit(RLIMIT_FSIZE, &limit);
    Viewer viewer;
    viewer.check = [](std::uint64_t) { return false; };
    auto writer = KeptFileWriter::create(file);
    std::string failed;
    viewer.on_kept = [&](const TimeShiftBuffer& kept) {
        if (failed.empty()) {
            if (auto written = writer->write_new(kept); !written) {
                failed = written.error();
            }
        }
    };
    feed_with_faults(viewer, 600);
    std::exit(failed.find("can't write") != std::string::npos && failed.find("too large") != std::string::npos ? 0 : 1);
}

// A write that fails (here, past the largest file the process may write) says so, with the reason,
// and the file is what was written before it.
TEST(KeptFile, AWriteThatFailsSaysWhy) {
    const Scratch scratch;
    const std::filesystem::path file = scratch.path / "kept";
    EXPECT_EXIT(write_past_the_limit(file), ::testing::ExitedWithCode(0), "");
    const auto read = read_kept_file(file);  // what was written before, whole
    ASSERT_TRUE(read) << read.error();
    EXPECT_FALSE(read->runs().empty());
}

TEST(BooksText, ListsEachBookAsTheViewerDoes) {
    Moment moment;
    moment.seq = 9;
    const auto order = [](std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty, std::uint64_t priority) {
        return MarketUpdate{.seq = 1, .type = UpdateType::add, .order_id = id, .ticker = 2, .side = side, .price = price, .qty = qty,
                            .priority = priority};
    };
    for (const MarketUpdate& add : {order(1, Side::buy, 100, 5, 1), order(2, Side::buy, 100, 7, 2), order(3, Side::buy, 99, 4, 1),
                                    order(10, Side::sell, 102, 3, 1), order(11, Side::sell, 101, 2, 1)}) {
        ASSERT_TRUE(moment.books[2].apply(add));
    }
    moment.last_trades[2] = MarketUpdate{.seq = 8, .type = UpdateType::trade, .order_id = 0, .ticker = 2, .side = Side::buy, .price = 101,
                                         .qty = 1, .priority = 0};
    EXPECT_EQ(books_text(moment, 2u),
              "ticker 2: 5 orders\n"
              "  sell      102  qty      3  order       10  #1 at its price (level qty 3)\n"
              "  sell      101  qty      2  order       11  #1 at its price (level qty 2)\n"
              "  buy       100  qty      5  order        1  #1 at its price (level qty 12)\n"
              "  buy       100  qty      7  order        2  #2 at its price\n"
              "  buy        99  qty      4  order        3  #1 at its price (level qty 4)\n"
              "  last trade: 1 bought at 101, update 8\n");
    const std::string all = books_text(moment, std::nullopt);
    EXPECT_TRUE(all.starts_with("ticker 2: 5 orders\n")) << all;
    EXPECT_TRUE(all.ends_with("tickers 0, 1, 3, 4, 5, 6, 7: no orders, no trade\n")) << all;
}

}  // namespace
}  // namespace fillscope
