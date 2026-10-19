// The stand-in frame source: frame generation, the command line, and a whole run against a
// listening socket.

#include "frame_source.h"
#include "lockstep/anchor.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <thread>

namespace fillscope {
namespace {

TEST(FrameGenerator, IndicesAreConsecutiveFromZero) {
    FrameGenerator generator{42};
    for (std::uint64_t i = 0; i < 1000; ++i) {
        EXPECT_EQ(generator.next().index, i);
    }
}

TEST(FrameGenerator, SameSeedGivesSameFrames) {
    FrameGenerator a{7};
    FrameGenerator b{7};
    FrameGenerator other{8};
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const Frame fa = a.next();
        EXPECT_EQ(fa, b.next());
        differs = differs || fa != other.next();
    }
    EXPECT_TRUE(differs) << "a different seed should give different frames";
}

TEST(FrameGenerator, PriceWalksInBoundedStepsAndQtyStaysInRange) {
    FrameGenerator generator{42};
    std::int64_t previous = FrameGenerator::kStartPrice;
    for (int i = 0; i < 1000; ++i) {
        const Frame frame = generator.next();
        EXPECT_LE(std::abs(frame.price - previous), FrameGenerator::kMaxPriceStep);
        EXPECT_GE(frame.qty, FrameGenerator::kMinQty);
        EXPECT_LE(frame.qty, FrameGenerator::kMaxQty);
        previous = frame.price;
    }
}

TEST(FrameGenerator, IdsStayInTheirPoolAndRecur) {
    FrameGenerator generator{42};
    std::array<int, FrameGenerator::kMaxId + 1> seen{};
    for (int i = 0; i < 1000; ++i) {
        const Frame frame = generator.next();
        ASSERT_GE(frame.id, FrameGenerator::kMinId);
        ASSERT_LE(frame.id, FrameGenerator::kMaxId);
        ++seen[frame.id];
    }
    // 1000 draws from 20 ids: every id appears, and recurs, so there is always something to follow.
    for (std::uint32_t id = FrameGenerator::kMinId; id <= FrameGenerator::kMaxId; ++id) {
        EXPECT_GT(seen[id], 1) << "id " << id;
    }
}

TEST(ParseSourceArgs, DefaultsAndEveryOption) {
    const auto defaults = parse_source_args({});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->frames, 1000u);
    EXPECT_FALSE(defaults->self_stop);
    EXPECT_EQ(defaults->socket_name, kDefaultSocketName);

    const std::array args{"--frames", "5", "--interval-ms", "0", "--socket", "s", "--seed", "9", "--self-stop"};
    const auto options = parse_source_args(args);
    ASSERT_TRUE(options);
    EXPECT_EQ(options->frames, 5u);
    EXPECT_EQ(options->interval.count(), 0);
    EXPECT_EQ(options->socket_name, "s");
    EXPECT_EQ(options->seed, 9u);
    EXPECT_TRUE(options->self_stop);
}

TEST(ParseSourceArgs, RejectsUnknownOrIncompleteArguments) {
    EXPECT_FALSE(parse_source_args(std::array{"--bogus"}));
    EXPECT_FALSE(parse_source_args(std::array{"--frames"}));
}

TEST(RunFrameSource, DescribesTheRunThenSendsEveryGeneratedFrameInOrder) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver{listen_on(name)};

    SourceOptions options;
    options.frames = 50;
    options.interval = std::chrono::milliseconds{0};
    options.socket_name = name;
    options.seed = 7;
    int exit_code = -1;
    std::thread source{[&] { exit_code = run_frame_source(options); }};

    std::vector<Message> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (received.size() < options.frames + 1 && std::chrono::steady_clock::now() < deadline) {
        receiver.drain(received);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    source.join();

    EXPECT_EQ(exit_code, 0);
    ASSERT_EQ(received.size(), options.frames + 1);
    // Parenthesized because a macro's arguments are split at commas, and braces don't protect them.
    EXPECT_EQ(received.front(), (Message{SourceHello{.seed = 7, .frame_count = 50}}));
    FrameGenerator expected{options.seed};
    for (std::size_t i = 1; i < received.size(); ++i) {
        EXPECT_EQ(received[i], Message{expected.next()});
    }
    EXPECT_EQ(last_anchored_frame(), options.frames - 1);
}

TEST(RunFrameSource, FailsWithoutAViewer) {
    SourceOptions options;
    options.socket_name = test::unique_socket_name();  // nobody listening
    options.connect_attempts = 1;  // the default waits five seconds for a viewer that won't come
    EXPECT_EQ(run_frame_source(options), 1);
}

}  // namespace
}  // namespace fillscope
