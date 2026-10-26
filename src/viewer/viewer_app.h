// The viewer's loop, one paint at a time: take in market data, draw, apply what the user did,
// publish.
//
// The viewer program (main.cpp) and the viewer's UI tests both run the loop through this class, so
// the tests exercise exactly the steps the program takes, in the same order.
#pragma once

#include "feed/feed_socket.h"
#include "feed/market_data_file.h"
#include "feed/market_update.h"
#include "feed/time_shift.h"
#include "viewer/context.h"
#include "viewer/context_publisher.h"
#include "viewer/viewer_state.h"
#include "viewer/viewer_view.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fillscope {

class ViewerApp {
public:
    using Clock = std::chrono::steady_clock;

    // At most this many datagrams are taken from each stream per paint, so a burst can't stall
    // drawing; what is left waits for the next paint.
    static constexpr std::size_t kMaxDatagramsPerPaint = 1024;

    explicit ViewerApp(std::filesystem::path context_file) : publisher_{std::move(context_file)} {}

    // Takes in what the market data streams have, once per paint: incremental updates first, then
    // snapshot updates (used only while recovering). The last error receiving or decoding is kept.
    void receive(FeedSocket& incremental, FeedSocket& snapshot, Clock::time_point now);

    // Applies one update, as receive() does for each; tests call these directly.
    void apply_incremental(const MarketUpdate& update, Clock::time_point now);
    void apply_snapshot(const MarketUpdate& update);

    // Shows a replay instead of the live exchange: the books start empty, before its first update.
    // `file` is where it was read from.
    void open_replay(const std::filesystem::path& file, MarketDataFile market_data);
    // In a replay, makes the books exactly the recorded run's after update `seq` (clamped to the
    // replay's length): forward by applying the updates after the books' last, back by rebuilding
    // from the first. What is pointed at and followed stays.
    void replay_to(std::uint64_t seq);

    // Shows answers: watches `file` for an answer (answer/answer.h), and shows each new one as it is
    // written. Nothing is shown until poll_answer() finds one.
    void watch_answer(const std::filesystem::path& file);
    // Looks at the answer file, at most once per kAnswerPollInterval: if it has been replaced since it
    // was last read, reads it, and shows the new answer from its start, or why it can't be shown.
    void poll_answer(Clock::time_point now);
    // Shows the answer's frame `frame` (clamped to its last): the books after that frame's update.
    void answer_to(std::size_t frame);
    // Offers `course`, its lessons' answers in `built`, beside the answer file watched (call after
    // watch_answer): that file is "your questions", choice 0.
    void open_course(Course course, const std::filesystem::path& built);
    // Shows choice `choice` (clamped): 0 for the user's own questions, otherwise that lesson, from its
    // start.
    void choose(std::size_t choice);
    static constexpr std::chrono::milliseconds kAnswerPollInterval{250};

    // One paint: draws the viewer (call between ImGui::NewFrame() and ImGui::Render()), applies what
    // the user did, and publishes the context, at once if the user did anything. Returns what the
    // user did.
    ViewerActions paint(Clock::time_point now);

    // As the viewer exits: publishes the final context, marked as from a writer no longer running.
    // Returns false if that write failed (publisher().error() says why).
    [[nodiscard]] bool finish();

    // A line for the program's own output when publishing starts failing, fails for a new reason,
    // or works again; otherwise nothing. The window shows the error too, but a run nobody watches
    // (with --exit-after, say) has only its output.
    [[nodiscard]] std::optional<std::string> publish_report();

    // What the viewer has kept of the live market data, every update applied with when it arrived
    // (feed/time_shift.h), from which the books after any of them can be shown.
    [[nodiscard]] const TimeShiftBuffer& kept() const noexcept { return kept_; }

    // The state, which tests also set directly (for example, the feed's error).
    [[nodiscard]] ViewerState& state() noexcept { return state_; }
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    [[nodiscard]] const ContextPublisher& publisher() const noexcept { return publisher_; }
    [[nodiscard]] std::uint64_t paints() const noexcept { return paints_; }

    // Replaces the publisher, so a test can publish somewhere writes fail.
    void set_publisher(std::filesystem::path context_file) { publisher_ = ContextPublisher{std::move(context_file)}; }

private:
    ViewerState state_;
    TimeShiftBuffer kept_;
    Selection selection_;
    ContextPublisher publisher_;
    std::optional<Clock::time_point> last_update_at_;
    std::uint64_t paints_ = 0;
    std::vector<MarketUpdate> received_;         // reused every paint, so receiving doesn't allocate
    std::optional<std::string> reported_error_;  // the publishing error last reported
    std::optional<Clock::time_point> answer_polled_at_;
};

}  // namespace fillscope
