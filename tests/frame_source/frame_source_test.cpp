// The stand-in frame source: the bounded draw, frame generation, the command line, and a whole run
// against a listening socket.

#include "frame_source.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <future>
#include <thread>

namespace fillscope {
namespace {

using namespace std::chrono_literals;

// ---- draw_uniform ----

// A generator whose draws the test chooses, satisfying std::uniform_random_bit_generator.
struct ScriptedGenerator {
    using result_type = std::uint64_t;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }
    std::vector<result_type> draws;
    std::size_t next = 0;
    result_type operator()() { return draws.at(next++); }
};

TEST(DrawUniform, RejectsTheDrawsThatWouldFavourSmallResults) {
    // For span 3, 2^64 mod 3 = 1: raw 0 is rejected and drawn again; raw 1 gives low + 1.
    ScriptedGenerator generator{.draws = {0, 1}};
    EXPECT_EQ(draw_uniform(generator, 10, 12), 11u);
    EXPECT_EQ(generator.next, 2u) << "the first draw should have been rejected";
}

TEST(DrawUniform, AcceptsEveryDrawWhenTheSpanDividesTwoToTheSixtyFour) {
    // For span 4, 2^64 mod 4 = 0: nothing is rejected.
    ScriptedGenerator generator{.draws = {0, 7}};
    EXPECT_EQ(draw_uniform(generator, 0, 3), 0u);
    EXPECT_EQ(draw_uniform(generator, 0, 3), 3u);
}

TEST(DrawUniform, TheFullRangeIsTheRawDraw) {
    ScriptedGenerator generator{.draws = {12345}};
    EXPECT_EQ(draw_uniform(generator, 0, std::numeric_limits<std::uint64_t>::max()), 12345u);
}

TEST(DrawUniform, StaysWithinItsBounds) {
    std::mt19937_64 engine{1};
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t value = draw_uniform(engine, 5, 9);
        ASSERT_GE(value, 5u);
        ASSERT_LE(value, 9u);
    }
}

// ---- FrameGenerator ----

TEST(FrameGenerator, TheEngineIsTheStandardOne) {
    // The C++ standard requires the 10000th draw of a default-constructed std::mt19937_64 to be
    // this value ([rand.predef]): the engine, at least, is the same everywhere.
    std::mt19937_64 engine;
    engine.discard(9999);
    EXPECT_EQ(engine(), 9981545732273789042u);
}

TEST(FrameGenerator, SeedFortyTwoGivesTheseFramesOnAnyCompiler) {
    // Golden values, computed independently by a Python implementation of MT19937-64 and the same
    // rejection method. A change to how frames are drawn changes what every replay address means,
    // so it has to fail here first.
    FrameGenerator generator{42};
    const std::array expected{
        Frame{.index = 0, .price = 9999, .qty = 325, .id = 11}, Frame{.index = 1, .price = 9999, .qty = 382, .id = 9},
        Frame{.index = 2, .price = 9998, .qty = 145, .id = 11}, Frame{.index = 3, .price = 9998, .qty = 96, .id = 3},
        Frame{.index = 4, .price = 9998, .qty = 407, .id = 10},
    };
    for (const Frame& frame : expected) {
        EXPECT_EQ(generator.next(), frame);
    }
}

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

// ---- parse_source_args ----

TEST(ParseSourceArgs, DefaultsAndEveryOption) {
    const auto defaults = parse_source_args({});
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->frames, 1000u);
    EXPECT_EQ(defaults->interval, 200ms);
    EXPECT_EQ(defaults->seed, 42u);
    EXPECT_FALSE(defaults->self_stop);
    EXPECT_EQ(defaults->socket_name, kDefaultSocketName);

    const std::array args{"--frames", "5", "--interval-ms", "0", "--socket", "s", "--seed", "9", "--self-stop"};
    const auto options = parse_source_args(args);
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->frames, 5u);
    EXPECT_EQ(options->interval, 0ms);
    EXPECT_EQ(options->socket_name, "s");
    EXPECT_EQ(options->seed, 9u);
    EXPECT_TRUE(options->self_stop);
}

TEST(ParseSourceArgs, RejectsUnknownOrIncompleteArgumentsAndSaysWhy) {
    EXPECT_EQ(parse_source_args(std::array{"--bogus"}).error(), "unknown option '--bogus'");
    EXPECT_EQ(parse_source_args(std::array{"--frames"}).error(), "--frames needs a value");
}

TEST(ParseSourceArgs, RejectsNumbersThatAreNotExactlyNumbers) {
    // Each of these was once accepted, read as something else, or ended the program.
    for (const char* bad : {"abc", "5x", "-1", "0x10", " 5", "", "99999999999999999999"}) {
        const std::array args{"--frames", bad};
        EXPECT_FALSE(parse_source_args(args).has_value()) << "--frames '" << bad << "'";
    }
    EXPECT_FALSE(parse_source_args(std::array{"--interval-ms", "-5"}).has_value());
    EXPECT_FALSE(parse_source_args(std::array{"--interval-ms", "99999999999"}).has_value());
    EXPECT_EQ(parse_source_args(std::array{"--seed", "5x"}).error(), "--seed: '5x' is not a number");
}

// ---- run_frame_source ----

// Runs the frame source on a thread; the future's wait_for keeps a hang from hanging the test.
std::future<int> start_source(const SourceOptions& options) {
    return std::async(std::launch::async, [options] { return run_frame_source(options); });
}

SourceOptions quick_options(const std::string& name, std::uint64_t frames) {
    SourceOptions options;
    options.frames = frames;
    options.interval = 0ms;
    options.socket_name = name;
    options.seed = 7;
    options.connect_attempts = 20;
    options.connect_retry = 10ms;
    return options;
}

TEST(RunFrameSource, DescribesTheRunThenSendsEveryGeneratedFrameInOrder) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const SourceOptions options = quick_options(name, 50);
    auto source = start_source(options);

    std::vector<Message> received;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (received.size() < options.frames + 1 && std::chrono::steady_clock::now() < deadline) {
        receiver.drain(received);
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(source.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(source.get(), 0);
    ASSERT_EQ(received.size(), options.frames + 1);
    // Parenthesized because a macro's arguments are split at commas, and braces don't protect them.
    EXPECT_EQ(received.front(), (Message{SourceHello{.seed = 7, .frame_count = 50}}));
    FrameGenerator expected{options.seed};
    for (std::size_t i = 1; i < received.size(); ++i) {
        EXPECT_EQ(received[i], Message{expected.next()});
    }
}

TEST(RunFrameSource, ZeroFramesSendsOnlyTheDescription) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    auto source = start_source(quick_options(name, 0));
    ASSERT_EQ(source.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(source.get(), 0);
    std::vector<Message> received;
    receiver.drain(received);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<SourceHello>(received[0]));
}

TEST(RunFrameSource, FailsWhenTheViewerGoesAway) {
    const std::string name = test::unique_socket_name();
    SourceOptions options = quick_options(name, 1'000'000);
    std::future<int> source;
    {
        MessageReceiver receiver = test::listen(name);
        source = start_source(options);
        std::vector<Message> received;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (received.empty() && std::chrono::steady_clock::now() < deadline) {
            receiver.drain(received);
        }
        ASSERT_FALSE(received.empty());
    }  // the viewer goes away mid-run
    ASSERT_EQ(source.wait_for(1s), std::future_status::ready) << "the source didn't notice";
    EXPECT_EQ(source.get(), 1);
}

TEST(RunFrameSource, FailsWithoutAViewer) {
    SourceOptions options = quick_options(test::unique_socket_name(), 1);  // nobody listening
    options.connect_attempts = 1;
    EXPECT_EQ(run_frame_source(options), 1);
}

}  // namespace
}  // namespace fillscope
