// The stand-in frame source: frame generation, the command line, and a whole run against a
// listening socket.

#include "frame_source.h"
#include "lockstep/anchor.h"
#include "test_support.h"

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

TEST(RunFrameSource, SendsEveryGeneratedFrameInOrder) {
    const std::string name = test::unique_socket_name();
    FrameReceiver receiver{listen_on(name)};

    SourceOptions options;
    options.frames = 50;
    options.interval = std::chrono::milliseconds{0};
    options.socket_name = name;
    int exit_code = -1;
    std::thread source{[&] { exit_code = run_frame_source(options); }};

    std::vector<Frame> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (received.size() < options.frames && std::chrono::steady_clock::now() < deadline) {
        receiver.drain(received);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    source.join();

    EXPECT_EQ(exit_code, 0);
    FrameGenerator expected{options.seed};
    ASSERT_EQ(received.size(), options.frames);
    for (const Frame& frame : received) {
        EXPECT_EQ(frame, expected.next());
    }
    EXPECT_EQ(last_anchored_frame(), options.frames - 1);
}

TEST(RunFrameSource, FailsWithoutAViewer) {
    SourceOptions options;
    options.socket_name = test::unique_socket_name();  // nobody listening
    EXPECT_EQ(run_frame_source(options), 1);
}

}  // namespace
}  // namespace fillscope
