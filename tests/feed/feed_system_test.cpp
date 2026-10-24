// Fillscope's books against the exchange's, with the real back end: the exchange (publishing a
// snapshot every second, where the book's has a minute) and a client trading at random. The test
// joins both market data streams before the exchange starts, so it has every update, and keeps
// what arrives, in order. Then it checks, from what it kept:
//   - the books built from incremental updates 1 to S equal the exchange's snapshot as of update S
//     (the books its snapshot synthesizer keeps), for every snapshot taken after trading began;
//   - a reader that joins the feed late recovers from a snapshot, and its books then equal those of
//     the reader that had every update;
//   - the exchange's recording of its requests, replayed (exchange_replay), gives byte for byte the
//     incremental updates that arrived: deterministic replay.
//
//   feed_system_tests EXCHANGE_MAIN TRADING_MAIN EXCHANGE_REPLAY
#include "feed/book_builder.h"
#include "feed/feed_socket.h"
#include "feed/market_data_file.h"
#include "feed/market_update.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <chrono>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fillscope {
namespace {

std::string g_exchange, g_trading, g_replay;

// Where this test's exchange meets its client and this test: ports of the test's own (test_port), so
// nothing else on the machine sending or listening on the exchange's usual ones (a lockstep session
// in VS Code, another run of the tests) reaches it. The groups are the exchange's own.
const std::uint16_t kOrderPort = test::test_port(0);
const Stream kIncremental{kExchangeIncremental.group, test::test_port(1)};
const Stream kSnapshot{kExchangeSnapshot.group, test::test_port(2)};

std::vector<std::string> endpoint_args() {
    return {"--order-port", std::to_string(kOrderPort), "--incremental", std::format("{}:{}", kIncremental.group, kIncremental.port),
            "--snapshot", std::format("{}:{}", kSnapshot.group, kSnapshot.port)};
}

std::vector<std::string> with_endpoints(std::vector<std::string> args) {
    const auto endpoints = endpoint_args();
    args.insert(args.end(), endpoints.begin(), endpoints.end());
    return args;
}

// Whether something listens on this test's order port.
bool order_port_open() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    const sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(kOrderPort), .sin_addr = {htonl(INADDR_LOOPBACK)}, .sin_zero = {}};
    const bool open = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    ::close(fd);
    return open;
}

// A directory of the test's own, removed however the test ends.
struct TemporaryDirectory {
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / std::format("fillscope-feed-{}", ::getpid());
};

// A back-end program, started in `directory` (it writes its logs there) and killed at the end: how
// it shuts down isn't the subject here, and takes a minute.
class Program {
public:
    Program(const std::vector<std::string>& argv, const std::filesystem::path& directory) {
        std::vector<char*> args;
        for (const auto& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
        // Its output would bury the test's; its logs are in `directory`.
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        started_ = posix_spawn(&pid_, args[0], &actions, nullptr, args.data(), environ) == 0;
        posix_spawn_file_actions_destroy(&actions);
    }
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    ~Program() {
        if (started_) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
    }
    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] bool running() const { return started_ && ::waitpid(pid_, nullptr, WNOHANG) == 0; }

private:
    pid_t pid_ = -1;
    bool started_ = false;
};

// Runs a program in `directory` to its end; its exit status, or -1.
int run_to_exit(const std::vector<std::string>& argv, const std::filesystem::path& directory) {
    std::vector<char*> args;
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const bool started = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    return started && ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// What arrived, in the order it arrived.
struct Arrival {
    bool snapshot = false;
    MarketUpdate update;
};

// The complete snapshots among what arrived: each a start numbered 0, every update after it in
// sequence, and an end.
std::vector<std::vector<MarketUpdate>> whole_snapshots(const std::vector<Arrival>& arrivals) {
    std::vector<std::vector<MarketUpdate>> snapshots;
    std::vector<MarketUpdate> current;
    for (const Arrival& arrival : arrivals) {
        if (!arrival.snapshot) {
            continue;
        }
        const MarketUpdate& update = arrival.update;
        if (update.type == UpdateType::snapshot_start) {
            current = {update};
        } else if (!current.empty() && update.seq == current.size()) {
            current.push_back(update);
            if (update.type == UpdateType::snapshot_end) {
                snapshots.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.clear();  // a gap: this one can't be used
        }
    }
    return snapshots;
}

TEST(FeedAgainstTheExchange, BooksEqualTheExchangesSnapshotsALateReaderRecoversAndTheRecordingReplays) {
    ASSERT_FALSE(order_port_open()) << "something is already listening on this test's order port, " << kOrderPort;
    const TemporaryDirectory temporary;
    const std::filesystem::path& directory = temporary.path;

    auto incremental = FeedSocket::join(kIncremental);
    auto snapshots = FeedSocket::join(kSnapshot);
    ASSERT_TRUE(incremental) << incremental.error().message();
    ASSERT_TRUE(snapshots) << snapshots.error().message();

    std::vector<Arrival> arrivals;
    {
        Program exchange(with_endpoints({g_exchange, "--snapshot-interval-seconds", "1", "--record", "requests.rec"}), directory);
        ASSERT_TRUE(exchange.started());
        std::this_thread::sleep_for(std::chrono::seconds{10});  // its parts start a couple of seconds apart
        ASSERT_TRUE(exchange.running()) << "the exchange exited";
        Program client(with_endpoints({g_trading, "5", "RANDOM"}), directory);
        ASSERT_TRUE(client.started());

        // Until three snapshots taken after trading began have arrived whole, or a minute has passed.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
        std::vector<MarketUpdate> received;
        for (;;) {
            const auto take = [&](FeedSocket& socket, bool snapshot) {
                received.clear();
                const auto result = socket.receive(received, 64);
                EXPECT_FALSE(result.error) << *result.error;
                for (const MarketUpdate& update : received) {
                    arrivals.push_back({snapshot, update});
                }
                return result.datagrams;
            };
            const auto datagrams = take(*incremental, false) + take(*snapshots, true);
            std::size_t after_trading = 0;
            for (const auto& snapshot : whole_snapshots(arrivals)) {
                after_trading += snapshot.front().order_id > 0;
            }
            if (after_trading >= 3) {
                break;
            }
            ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "only " << after_trading << " snapshots after trading began";
            ASSERT_TRUE(exchange.running()) << "the exchange stopped";
            if (datagrams == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
    }  // both killed

    // The recording, replayed. The exchange was killed while running, so the recording may hold
    // requests whose updates were published after the test stopped listening: the replay may be
    // longer, and must match everything that arrived.
    ASSERT_EQ(run_to_exit({g_replay, "requests.rec", "market.dat"}, directory), 0);
    const auto replayed = read_market_data_file(directory / "market.dat");
    ASSERT_TRUE(replayed) << replayed.error();

    // Every incremental update arrived, numbered from 1 without a gap: nothing was lost.
    std::vector<MarketUpdate> stream;
    for (const Arrival& arrival : arrivals) {
        if (!arrival.snapshot) {
            stream.push_back(arrival.update);
        }
    }
    ASSERT_FALSE(stream.empty());
    for (std::size_t i = 0; i < stream.size(); ++i) {
        ASSERT_EQ(stream[i].seq, i + 1) << "an update was lost";
    }

    // Deterministic replay: the replay's updates are those that arrived, byte for byte.
    ASSERT_GE(replayed->updates.size(), stream.size());
    for (std::size_t i = 0; i < stream.size(); ++i) {
        ASSERT_EQ(encode_update(replayed->updates[i]), encode_update(stream[i])) << "update " << i + 1 << " differs";
    }
    EXPECT_EQ(replayed->recording, "requests.rec");

    // The books from updates 1 to S equal each snapshot as of S.
    std::size_t compared = 0;
    for (const auto& snapshot : whole_snapshots(arrivals)) {
        const std::uint64_t last = snapshot.front().order_id;
        if (last == 0 || last > stream.size()) {
            continue;
        }
        BookBuilder builder;
        for (std::uint64_t seq = 1; seq <= last; ++seq) {
            builder.on_incremental(stream[seq - 1]);
        }
        ASSERT_EQ(builder.status(), BookBuilder::Status::live) << *builder.recovering_because();
        std::array<Book, BookBuilder::kTickers> expected;
        for (const MarketUpdate& update : snapshot) {
            if (update.type == UpdateType::add) {
                ASSERT_TRUE(expected.at(update.ticker).apply(update)) << "the snapshot as of " << last;
            }
        }
        for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
            EXPECT_EQ(builder.book(ticker).orders(), expected[ticker].orders()) << "ticker " << ticker << ", as of update " << last;
        }
        ++compared;
    }
    EXPECT_GE(compared, 3u);

    // A reader that had everything, and one that joined after a third of the incremental updates
    // had arrived, fed what arrived in the order it arrived.
    BookBuilder early, late;
    const std::uint64_t join_at = stream.size() / 3 + 1;
    for (const Arrival& arrival : arrivals) {
        if (arrival.snapshot) {
            early.on_snapshot(arrival.update);
            late.on_snapshot(arrival.update);
        } else {
            early.on_incremental(arrival.update);
            if (arrival.update.seq >= join_at) {
                late.on_incremental(arrival.update);
            }
        }
    }
    EXPECT_EQ(early.status(), BookBuilder::Status::live);
    EXPECT_TRUE(early.recoveries().empty());
    ASSERT_EQ(late.status(), BookBuilder::Status::live) << *late.recovering_because();
    ASSERT_EQ(late.recoveries().size(), 1u);
    EXPECT_EQ(late.last_seq(), early.last_seq());
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        EXPECT_EQ(late.book(ticker).orders(), early.book(ticker).orders()) << "ticker " << ticker;
    }
}

}  // namespace
}  // namespace fillscope

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc != 4) {
        std::fprintf(stderr, "usage: feed_system_tests EXCHANGE_MAIN TRADING_MAIN EXCHANGE_REPLAY\n");
        return 2;
    }
    fillscope::g_exchange = argv[1];
    fillscope::g_trading = argv[2];
    fillscope::g_replay = argv[3];
    return RUN_ALL_TESTS();
}
