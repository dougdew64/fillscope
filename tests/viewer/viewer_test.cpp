// The viewer's core, without Dear ImGui: its command line (parse_viewer_args), what it has received
// (ViewerState, Run, PriceEnvelope), what the user has pointed at and followed (Selection), the
// context built from both (make_context, to_json, bar_text), and the file it is published to
// (ContextPublisher, write_atomically).

#include "viewer/context.h"
#include "viewer/context_publisher.h"
#include "viewer/viewer_options.h"
#include "viewer/viewer_state.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>

namespace fillscope {
namespace {

using namespace std::chrono_literals;

Frame frame(std::uint64_t index, std::uint32_t id, std::int64_t price = 10'000) {
    return Frame{.index = index, .price = price, .qty = 5, .id = id};
}

// A state holding one described run (seed 42) with the given frames.
ViewerState described_run(std::initializer_list<Frame> frames) {
    ViewerState state;
    state.apply(SourceHello{.seed = 42, .frame_count = 100});
    for (const Frame& f : frames) {
        state.apply(f);
    }
    state.source_connected = true;
    return state;
}

const Writer kWriter{.pid = 1234, .started_at_ms = 5, .running = true};

// ---- parse_viewer_args ----

TEST(ParseViewerArgs, DefaultsAndEveryOption) {
    const auto defaults = parse_viewer_args({});
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->exit_after_seconds, 0.0);
    EXPECT_EQ(defaults->socket_name, kDefaultSocketName);
    EXPECT_EQ(defaults->context_file, kDefaultContextFile);

    const std::array args{"--exit-after", "2.5", "--socket", "s", "--context-file", "c.json"};
    const auto options = parse_viewer_args(args);
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->exit_after_seconds, 2.5);
    EXPECT_EQ(options->socket_name, "s");
    EXPECT_EQ(options->context_file, "c.json");
}

TEST(ParseViewerArgs, RefusesWhatIsntAnOptionOrANumberOfSeconds) {
    EXPECT_EQ(parse_viewer_args(std::array{"--bogus"}).error(), "unknown option '--bogus'");
    EXPECT_EQ(parse_viewer_args(std::array{"--socket"}).error(), "--socket needs a value");
    for (const char* bad : {"abc", "2s", "-1", "nan", ""}) {
        const std::array args{"--exit-after", bad};
        EXPECT_FALSE(parse_viewer_args(args).has_value()) << "--exit-after '" << bad << "'";
    }
    EXPECT_FALSE(parse_viewer_args(std::array{"--context-file", ""}).has_value());
}

// ---- ViewerState and Run ----

TEST(ViewerState, AHelloStartsANewRunAndDropsTheOldFrames) {
    ViewerState state = described_run({frame(0, 1), frame(1, 2)});
    EXPECT_TRUE(state.apply(SourceHello{.seed = 7, .frame_count = 5}));
    ASSERT_TRUE(state.run);
    EXPECT_TRUE(state.run->frames().empty());
    EXPECT_EQ(state.run->hello()->seed, 7u);
}

TEST(ViewerState, AHelloOnAnEmptyStateStartsARun) {
    ViewerState state;
    EXPECT_TRUE(state.apply(SourceHello{.seed = 3, .frame_count = 1}));
    ASSERT_TRUE(state.run);
    EXPECT_EQ(state.run->hello()->seed, 3u);
}

TEST(ViewerState, FramesWithoutAHelloFormARunWithNoDescription) {
    ViewerState state;
    EXPECT_TRUE(state.apply(frame(0, 1)));   // the first frame starts a run
    EXPECT_FALSE(state.apply(frame(1, 1)));  // later frames join it
    ASSERT_TRUE(state.run);
    EXPECT_FALSE(state.run->hello());
    EXPECT_EQ(state.run->frames().size(), 2u);
}

TEST(ViewerState, ANewConnectionNeverInheritsTheLastSourcesRun) {
    // A source that doesn't describe itself, after one that did: its frames must not join the old
    // run, or they would claim the old run's seed.
    ViewerState state = described_run({frame(0, 1), frame(1, 1)});
    state.begin_connection();
    EXPECT_FALSE(state.run);
    EXPECT_TRUE(state.apply(frame(0, 5)));
    EXPECT_FALSE(state.run->hello()) << "no seed: this source never gave one";
    EXPECT_EQ(state.run->frames().size(), 1u);
}

TEST(Run, IndexesEachIdsFrames) {
    const ViewerState state = described_run({frame(0, 7), frame(1, 2), frame(2, 7)});
    EXPECT_EQ(std::vector(std::from_range, state.run->indices_of(7)), (std::vector<std::uint64_t>{0, 2}));
    EXPECT_EQ(std::vector(std::from_range, state.run->indices_of(2)), (std::vector<std::uint64_t>{1}));
    EXPECT_TRUE(state.run->indices_of(19).empty());
}

// ---- PriceEnvelope ----

TEST(PriceEnvelope, OneBucketPerPriceUntilFull) {
    PriceEnvelope envelope;
    envelope.append(3);
    envelope.append(1);
    ASSERT_EQ(envelope.buckets().size(), 2u);
    EXPECT_EQ(envelope.buckets()[0], (PriceEnvelope::Bucket{.low = 3, .high = 3}));
    EXPECT_EQ(envelope.frames_per_bucket(), 1u);
}

TEST(PriceEnvelope, StaysBoundedAndKeepsEverySpike) {
    PriceEnvelope envelope;
    constexpr std::size_t kPrices = 100'000;
    for (std::size_t i = 0; i < kPrices; ++i) {
        // A flat line with one spike up and one down, far apart.
        const float price = i == 31'337 ? 500.0f : i == 77'777 ? -400.0f : 10.0f;
        envelope.append(price);
    }
    EXPECT_LE(envelope.buckets().size(), PriceEnvelope::kMaxBuckets);
    // Every price is in exactly one bucket: the buckets cover all of them.
    EXPECT_GE(envelope.buckets().size() * envelope.frames_per_bucket(), kPrices);
    const auto highest = std::ranges::max(envelope.buckets(), {}, &PriceEnvelope::Bucket::high).high;
    const auto lowest = std::ranges::min(envelope.buckets(), {}, &PriceEnvelope::Bucket::low).low;
    EXPECT_EQ(highest, 500.0f);
    EXPECT_EQ(lowest, -400.0f);
}

TEST(PriceEnvelope, ReducedKeepsTheExtremesInFewerBuckets) {
    PriceEnvelope envelope;
    for (int i = 0; i < 1000; ++i) {
        envelope.append(i == 500 ? 99.0f : 0.0f);
    }
    const auto reduced = envelope.reduced(10);
    EXPECT_LE(reduced.size(), 10u);
    EXPECT_EQ(std::ranges::max(reduced, {}, &PriceEnvelope::Bucket::high).high, 99.0f);
}

// ---- Selection ----

TEST(Selection, EachChangeTakesTheNextNumberFromOneCounter) {
    Selection selection;
    selection.point_at(frame(3, 1));
    selection.follow(7);
    EXPECT_LT(selection.point_seq(), selection.follow_seq()) << "followed after pointing";
    selection.clear_point();
    EXPECT_GT(selection.point_seq(), selection.follow_seq()) << "cleared the point last";
    EXPECT_FALSE(selection.pointed());
    EXPECT_EQ(selection.followed(), 7u);
}

TEST(Selection, ANewRunClearsThePointButKeepsTheFollow) {
    Selection selection;
    selection.point_at(frame(3, 1));
    selection.follow(7);
    selection.new_run();
    EXPECT_FALSE(selection.pointed());
    EXPECT_EQ(selection.followed(), 7u);
}

TEST(Selection, ANewRunTakesNoSequenceNumberBecauseTheUserDidNothing) {
    Selection selection;
    selection.point_at(frame(3, 1));
    selection.follow(7);
    const auto point_seq = selection.point_seq();
    selection.new_run();
    EXPECT_EQ(selection.point_seq(), point_seq);
    EXPECT_GT(selection.follow_seq(), selection.point_seq()) << "following is still the user's last act";
}

// ---- make_context ----

TEST(MakeContext, ThePointHasAReplayAddressFromTheRunsSeed) {
    const ViewerState state = described_run({frame(0, 1), frame(1, 2)});
    Selection selection;
    selection.point_at(frame(1, 2));
    const Context context = make_context(state, selection);
    ASSERT_TRUE(context.point);
    EXPECT_EQ(context.point->frame, frame(1, 2));
    EXPECT_EQ(context.point->replay, (ReplayAddress{.seed = 42, .frame_index = 1}));
    EXPECT_EQ(context.point->seq, selection.point_seq());
}

TEST(MakeContext, WithoutADescriptionThereIsNoReplayAddress) {
    ViewerState state;
    state.apply(frame(0, 1));
    Selection selection;
    selection.point_at(frame(0, 1));
    selection.follow(1);
    const Context context = make_context(state, selection);
    EXPECT_FALSE(context.point->replay);
    EXPECT_FALSE(context.follow->seed);
}

TEST(MakeContext, FollowListsEveryFrameWithTheIdAndCountsThem) {
    const ViewerState state = described_run({frame(0, 7), frame(1, 2), frame(2, 7), frame(3, 7), frame(4, 9)});
    Selection selection;
    selection.follow(7);
    const Context context = make_context(state, selection);
    ASSERT_TRUE(context.follow);
    EXPECT_EQ(context.follow->frame_indices, (std::vector<std::uint64_t>{0, 2, 3}));
    EXPECT_EQ(context.follow->frame_count, 3u);
    EXPECT_FALSE(context.follow->truncated());
    EXPECT_EQ(context.follow->seed, 42u);
}

TEST(MakeContext, AFollowedIdThatNeverAppearsHasNoFrames) {
    const ViewerState state = described_run({frame(0, 1)});
    Selection selection;
    selection.follow(19);
    const Context context = make_context(state, selection);
    EXPECT_TRUE(context.follow->frame_indices.empty());
    EXPECT_EQ(context.follow->frame_count, 0u);
}

TEST(MakeContext, AFollowListIsCappedButItsCountIsExact) {
    ViewerState state;
    state.apply(SourceHello{.seed = 1, .frame_count = 1500});
    for (std::uint64_t i = 0; i < 1500; ++i) {
        state.apply(frame(i, 4));
    }
    Selection selection;
    selection.follow(4);
    const Context context = make_context(state, selection);
    EXPECT_EQ(context.follow->frame_indices.size(), kMaxFollowedIndices);
    EXPECT_EQ(context.follow->frame_count, 1500u);
    EXPECT_TRUE(context.follow->truncated());
}

TEST(MakeContext, TheBackgroundDescribesTheSourceAndTheLatestFrame) {
    ViewerState state = described_run({frame(0, 1), frame(1, 1)});
    state.source_error = "an empty message";
    const Context context = make_context(state, Selection{});
    EXPECT_TRUE(context.source.connected);
    EXPECT_EQ(context.source.run, (SourceHello{.seed = 42, .frame_count = 100}));
    EXPECT_EQ(context.source.frames_received, 2u);
    EXPECT_EQ(context.source.latest_frame_index, 1u);
    EXPECT_EQ(context.source.error, "an empty message");
    EXPECT_FALSE(context.point);
    EXPECT_FALSE(context.follow);
}

// ---- to_json ----

TEST(ContextJson, EmptySectionsAreNullAndEveryFieldIsThere) {
    const nlohmann::ordered_json json = to_json(make_context(ViewerState{}, Selection{}), kWriter, 3, 1234);
    EXPECT_EQ(json["version"], 1);
    EXPECT_EQ(json["seq"], 3);
    EXPECT_EQ(json["writtenAtMs"], 1234);
    EXPECT_TRUE(json["point"].is_null());
    EXPECT_TRUE(json["follow"].is_null());
    EXPECT_TRUE(json["background"]["source"]["run"].is_null());
    EXPECT_TRUE(json["background"]["source"]["error"].is_null());
    EXPECT_EQ(json["background"]["framesReceived"], 0);
    EXPECT_TRUE(json["background"]["latestFrameIndex"].is_null());
}

TEST(ContextJson, SaysWhoWroteItAndWhetherItIsStillRunning) {
    const nlohmann::ordered_json json = to_json(make_context(ViewerState{}, Selection{}), kWriter, 1, 0);
    EXPECT_EQ(json["writer"]["program"], "viewer");
    EXPECT_EQ(json["writer"]["pid"], 1234);
    EXPECT_EQ(json["writer"]["startedAtMs"], 5);
    EXPECT_EQ(json["writer"]["running"], true);
}

TEST(ContextJson, PointAndFollowCarryTheirFramesAndReplayAddresses) {
    const ViewerState state = described_run({frame(0, 7, 10'003), frame(1, 2), frame(2, 7)});
    Selection selection;
    selection.point_at(frame(0, 7, 10'003));
    selection.follow(7);
    const nlohmann::ordered_json json = to_json(make_context(state, selection), kWriter, 1, 0);

    EXPECT_EQ(json["background"]["latestFrameIndex"], 2);
    const auto& point = json["point"];
    EXPECT_EQ(point["frame"]["index"], 0);
    EXPECT_EQ(point["frame"]["id"], 7);
    EXPECT_EQ(point["frame"]["price"], 10'003);
    EXPECT_EQ(point["replayAddress"]["seed"], 42);
    EXPECT_EQ(point["replayAddress"]["frameIndex"], 0);
    EXPECT_EQ(point["seq"], selection.point_seq());

    const auto& follow = json["follow"];
    EXPECT_EQ(follow["id"], 7);
    EXPECT_EQ(follow["frameIndices"], (std::vector<int>{0, 2}));
    EXPECT_EQ(follow["frameCount"], 2);
    EXPECT_EQ(follow["frameIndicesTruncated"], false);
    EXPECT_EQ(follow["seed"], 42);
    EXPECT_EQ(follow["seq"], selection.follow_seq());
}

TEST(ContextJson, AMissingReplayAddressSaysWhy) {
    ViewerState state;
    state.apply(frame(0, 1));
    Selection selection;
    selection.point_at(frame(0, 1));
    const nlohmann::ordered_json json = to_json(make_context(state, selection), kWriter, 1, 0);
    EXPECT_TRUE(json["point"]["replayAddress"].is_null());
    EXPECT_TRUE(json["point"]["replayAddressMissing"].is_string());
}

// ---- bar_text ----

TEST(ContextBar, SaysWhatTheContextHolds) {
    const ViewerState state = described_run({frame(0, 7, 10'003), frame(1, 2), frame(2, 7)});
    Selection selection;
    selection.point_at(frame(0, 7, 10'003));
    selection.follow(7);
    const ContextBarText text = bar_text(make_context(state, selection));
    EXPECT_EQ(text.background, "source seed 42 · 3 of 100 frames · connected");
    EXPECT_EQ(text.point, "Pointing at frame #0 (id 7, price 10003, qty 5)");
    EXPECT_EQ(text.follow, "Following id 7: 2 frames");
}

TEST(ContextBar, ShowsNoPointOrFollowLineWhenThereIsNone) {
    const ContextBarText text = bar_text(make_context(ViewerState{}, Selection{}));
    EXPECT_EQ(text.background, "no frames yet · not connected");
    EXPECT_FALSE(text.point);
    EXPECT_FALSE(text.follow);
}

TEST(ContextBar, SaysWhenTheSourceDidntDescribeItsRunAndThereIsNoReplayAddress) {
    ViewerState state;
    state.apply(frame(0, 3));
    Selection selection;
    selection.point_at(frame(0, 3));
    const ContextBarText text = bar_text(make_context(state, selection));
    EXPECT_EQ(text.background, "a source that didn't describe its run · 1 frames · not connected");
    EXPECT_EQ(text.point, "Pointing at frame #0 (id 3, price 10000, qty 5) · no replay address");
}

TEST(ContextBar, SaysWhyTheLastSourceWasDropped) {
    ViewerState state;
    state.source_error = "an empty message";
    EXPECT_EQ(bar_text(make_context(state, Selection{})).background,
              "no frames yet · not connected (the last source was dropped: an empty message)");
}

TEST(ContextBar, CountsOneFrameInTheSingularAndSaysWhenTheListIsCut) {
    ViewerState state = described_run({frame(0, 9)});
    Selection selection;
    selection.follow(9);
    EXPECT_EQ(bar_text(make_context(state, selection)).follow, "Following id 9: 1 frame");

    for (std::uint64_t i = 1; i < 1200; ++i) {
        state.apply(frame(i, 9));
    }
    EXPECT_EQ(bar_text(make_context(state, selection)).follow, "Following id 9: 1200 frames (first 1000 listed)");
}

// ---- ContextPublisher and write_atomically ----

// A fresh directory for each test, removed afterwards.
class Publisher : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               std::format("fillscope-context-{}-{}", ::getpid(),
                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] std::filesystem::path file() const { return dir_ / ".fillscope" / "context.json"; }

    [[nodiscard]] nlohmann::json read_file() const {
        std::ifstream in{file()};
        return nlohmann::json::parse(in);
    }

    [[nodiscard]] std::ptrdiff_t entries_in(const std::filesystem::path& directory) const {
        return std::ranges::distance(std::filesystem::directory_iterator{directory}, std::filesystem::directory_iterator{});
    }

    // A path every write to fails: a directory can't be created inside a regular file.
    [[nodiscard]] std::filesystem::path unwritable() const {
        std::filesystem::create_directories(dir_);
        std::ofstream{dir_ / "not-a-directory"} << "x";
        return dir_ / "not-a-directory" / "context.json";
    }

    std::filesystem::path dir_;
    const ContextPublisher::Clock::time_point t0_{};
};

Context pointing_at(std::uint64_t index) {
    const ViewerState state = described_run({frame(0, 1), frame(1, 1), frame(2, 1)});
    Selection selection;
    selection.point_at(frame(index, 1));
    return make_context(state, selection);
}

TEST_F(Publisher, AUserActionIsWrittenAtOnce) {
    ContextPublisher publisher{file(), 100ms};
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_);
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_ + 1ms);  // well inside the interval
    EXPECT_EQ(publisher.seq(), 2u);
    EXPECT_EQ(read_file()["point"]["frame"]["index"], 1);
}

TEST_F(Publisher, ABackgroundChangeWaitsForTheInterval) {
    ContextPublisher publisher{file(), 100ms};
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_);
    const Context later = make_context(described_run({frame(0, 1), frame(1, 1), frame(2, 1), frame(3, 1)}), Selection{});

    publisher.update(later, PublishTrigger::background, t0_ + 50ms);
    EXPECT_EQ(publisher.seq(), 1u) << "held back: too soon";
    EXPECT_NE(publisher.published(), later);

    publisher.update(later, PublishTrigger::background, t0_ + 100ms);
    EXPECT_EQ(publisher.seq(), 2u) << "written once the interval passed";
    EXPECT_EQ(publisher.published(), later);
}

TEST_F(Publisher, AnUnchangedContextIsNotRewritten) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_);
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_ + 1s);
    EXPECT_EQ(publisher.seq(), 1u);
}

TEST_F(Publisher, TheFileHoldsExactlyWhatPublishedSays) {
    ContextPublisher publisher{file()};
    const auto wall_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    const auto before = wall_ms();
    publisher.update(pointing_at(2), PublishTrigger::user_action, t0_);
    const auto after = wall_ms();

    const nlohmann::json written = read_file();
    const auto written_at = written["writtenAtMs"].get<std::int64_t>();
    EXPECT_GE(written_at, before);
    EXPECT_LE(written_at, after);
    const auto expected = to_json(*publisher.published(), publisher.writer(), publisher.seq(), written_at);
    EXPECT_EQ(written, nlohmann::json::parse(expected.dump()));
    EXPECT_EQ(written["writer"]["pid"], ::getpid());
    EXPECT_EQ(written["writer"]["running"], true);
}

TEST_F(Publisher, FinishSaysTheWriterIsNoLongerRunning) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_);
    publisher.finish(pointing_at(0));
    EXPECT_EQ(read_file()["writer"]["running"], false);
    EXPECT_EQ(publisher.seq(), 2u);
}

TEST_F(Publisher, NoTemporaryFileIsLeftBehind) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_);
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    EXPECT_EQ(entries_in(file().parent_path()), 1);
}

TEST_F(Publisher, AFailedWriteKeepsWhatTheFileHoldsAndSaysWhy) {
    ContextPublisher publisher{unwritable()};
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_);
    EXPECT_FALSE(publisher.published()) << "nothing was written, so nothing is published";
    ASSERT_TRUE(publisher.error());
    EXPECT_NE(publisher.error()->find("can't create"), std::string::npos) << *publisher.error();
    EXPECT_NE(publisher.error()->find("Not a directory"), std::string::npos) << "the reason, not just the fact";
    EXPECT_EQ(publisher.seq(), 0u);
}

TEST_F(Publisher, AFailedWriteIsRetriedAndRecovers) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(0), PublishTrigger::user_action, t0_);
    ASSERT_EQ(publisher.seq(), 1u);

    // Make the next write fail: the file's place is taken by a directory, which rename won't replace.
    std::filesystem::remove(file());
    std::filesystem::create_directories(file() / "blocker");
    publisher.update(pointing_at(1), PublishTrigger::user_action, t0_);
    ASSERT_TRUE(publisher.error());
    EXPECT_EQ(publisher.published(), pointing_at(0)) << "still what was last written successfully";
    EXPECT_EQ(entries_in(file().parent_path()), 1) << "no temporary file left after the failed rename";

    std::filesystem::remove_all(file());
    publisher.update(pointing_at(1), PublishTrigger::background, t0_ + 1s);  // retried, though unchanged
    EXPECT_FALSE(publisher.error());
    EXPECT_EQ(publisher.seq(), 2u) << "one more successful write";
    EXPECT_EQ(read_file()["point"]["frame"]["index"], 1);
}

TEST_F(Publisher, ABareFileNameIsWrittenInTheWorkingDirectory) {
    std::filesystem::create_directories(dir_);
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(dir_);
    const auto written = write_atomically("bare.json", "{}\n");
    std::filesystem::current_path(previous);
    ASSERT_TRUE(written.has_value()) << written.error();
    EXPECT_TRUE(std::filesystem::exists(dir_ / "bare.json"));
}

}  // namespace
}  // namespace fillscope
