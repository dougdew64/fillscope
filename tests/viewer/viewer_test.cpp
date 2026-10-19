// The viewer's core, without Dear ImGui: what it has received (ViewerState), what the user has
// pointed at and followed (Selection), the context built from both (make_context, to_json,
// bar_text), and the file it is published to (ContextPublisher).

#include "viewer/context.h"
#include "viewer/context_publisher.h"
#include "viewer/viewer_state.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <ranges>

namespace fillscope {
namespace {

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

// ---- ViewerState ----

TEST(ViewerState, AHelloStartsANewRunAndDropsTheOldFrames) {
    ViewerState state = described_run({frame(0, 1), frame(1, 2)});
    EXPECT_TRUE(state.apply(SourceHello{.seed = 7, .frame_count = 5}));
    ASSERT_TRUE(state.run);
    EXPECT_TRUE(state.run->frames().empty());
    EXPECT_EQ(state.run->hello()->seed, 7u);
}

TEST(ViewerState, FramesWithoutAHelloFormARunWithNoDescription) {
    ViewerState state;
    EXPECT_TRUE(state.apply(frame(0, 1)));   // the first frame starts a run
    EXPECT_FALSE(state.apply(frame(1, 1)));  // later frames join it
    ASSERT_TRUE(state.run);
    EXPECT_FALSE(state.run->hello());
    EXPECT_EQ(state.run->frames().size(), 2u);
}

TEST(ViewerState, PricesStayOnePerFrameInOrder) {
    const ViewerState state = described_run({frame(0, 1, 10'001), frame(1, 1, 9'999)});
    ASSERT_EQ(state.run->prices().size(), 2u);
    EXPECT_EQ(state.run->prices()[0], 10'001.0f);
    EXPECT_EQ(state.run->prices()[1], 9'999.0f);
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
    const ViewerState state = described_run({frame(0, 1), frame(1, 1)});
    const Context context = make_context(state, Selection{});
    EXPECT_TRUE(context.source.connected);
    EXPECT_EQ(context.source.run, (SourceHello{.seed = 42, .frame_count = 100}));
    EXPECT_EQ(context.source.frames_received, 2u);
    EXPECT_EQ(context.source.latest_frame_index, 1u);
    EXPECT_FALSE(context.point);
    EXPECT_FALSE(context.follow);
}

// ---- to_json and bar_text ----

TEST(ContextJson, EmptySectionsAreNullAndEveryFieldIsThere) {
    const nlohmann::ordered_json json = to_json(make_context(ViewerState{}, Selection{}), 3, 1234);
    EXPECT_EQ(json["version"], 1);
    EXPECT_EQ(json["seq"], 3);
    EXPECT_EQ(json["writtenAtMs"], 1234);
    EXPECT_TRUE(json["point"].is_null());
    EXPECT_TRUE(json["follow"].is_null());
    EXPECT_TRUE(json["background"]["source"]["run"].is_null());
    EXPECT_EQ(json["background"]["framesReceived"], 0);
    EXPECT_TRUE(json["background"]["latestFrameIndex"].is_null());
}

TEST(ContextJson, PointAndFollowCarryTheirFramesAndReplayAddresses) {
    const ViewerState state = described_run({frame(0, 7, 10'003), frame(1, 2), frame(2, 7)});
    Selection selection;
    selection.point_at(frame(0, 7, 10'003));
    selection.follow(7);
    const nlohmann::ordered_json json = to_json(make_context(state, selection), 1, 0);

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
    const nlohmann::ordered_json json = to_json(make_context(state, selection), 1, 0);
    EXPECT_TRUE(json["point"]["replayAddress"].is_null());
    EXPECT_TRUE(json["point"]["replayAddressMissing"].is_string());
}

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

// ---- ContextPublisher ----

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
    ContextPublisher publisher{file(), std::chrono::milliseconds{100}};
    publisher.update(pointing_at(0), true, t0_);
    publisher.update(pointing_at(1), true, t0_ + std::chrono::milliseconds{1});  // well inside the interval
    EXPECT_EQ(publisher.seq(), 2u);
    EXPECT_EQ(read_file()["point"]["frame"]["index"], 1);
}

TEST_F(Publisher, AChangeTheUserDidntMakeWaitsForTheInterval) {
    ContextPublisher publisher{file(), std::chrono::milliseconds{100}};
    publisher.update(pointing_at(0), true, t0_);
    const Context later = make_context(described_run({frame(0, 1), frame(1, 1), frame(2, 1), frame(3, 1)}), Selection{});

    publisher.update(later, false, t0_ + std::chrono::milliseconds{50});
    EXPECT_EQ(publisher.seq(), 1u) << "held back: too soon";
    EXPECT_NE(publisher.published(), later);

    publisher.update(later, false, t0_ + std::chrono::milliseconds{100});
    EXPECT_EQ(publisher.seq(), 2u) << "written once the interval passed";
    EXPECT_EQ(publisher.published(), later);
}

TEST_F(Publisher, AnUnchangedContextIsNotRewritten) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(0), true, t0_);
    publisher.update(pointing_at(0), true, t0_ + std::chrono::seconds{1});
    EXPECT_EQ(publisher.seq(), 1u);
}

TEST_F(Publisher, TheFileHoldsExactlyWhatPublishedSays) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(2), true, t0_);
    const nlohmann::json written = read_file();
    const auto expected = to_json(*publisher.published(), publisher.seq(), written["writtenAtMs"].get<std::int64_t>());
    EXPECT_EQ(written, nlohmann::json::parse(expected.dump()));
}

TEST_F(Publisher, NoTemporaryFileIsLeftBehind) {
    ContextPublisher publisher{file()};
    publisher.update(pointing_at(0), true, t0_);
    publisher.update(pointing_at(1), true, t0_);
    const auto entries = std::ranges::distance(std::filesystem::directory_iterator{file().parent_path()},
                                               std::filesystem::directory_iterator{});
    EXPECT_EQ(entries, 1);
}

TEST_F(Publisher, AFailedWriteKeepsWhatTheFileHoldsAndSaysWhy) {
    // A directory can't be created inside a regular file, so every write to this path fails.
    std::filesystem::create_directories(dir_);
    std::ofstream{dir_ / "not-a-directory"} << "x";
    ContextPublisher publisher{dir_ / "not-a-directory" / "context.json"};

    publisher.update(pointing_at(0), true, t0_);
    EXPECT_FALSE(publisher.published()) << "nothing was written, so nothing is published";
    ASSERT_TRUE(publisher.error());
    EXPECT_NE(publisher.error()->find("can't create"), std::string::npos) << *publisher.error();
    EXPECT_EQ(publisher.seq(), 0u);
}

}  // namespace
}  // namespace fillscope
