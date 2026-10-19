// The viewer's loop, one paint at a time: take in messages, draw, apply what the user did, publish.
//
// The viewer program (main.cpp) and the viewer's UI tests both run the loop through this class, so
// the tests exercise exactly the steps the program takes, in the same order.
#pragma once

#include "channel/net.h"
#include "viewer/context.h"
#include "viewer/context_publisher.h"
#include "viewer/viewer_state.h"
#include "viewer/viewer_view.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fillscope {

class ViewerApp {
public:
    using Clock = std::chrono::steady_clock;

    explicit ViewerApp(std::filesystem::path context_file) : publisher_{std::move(context_file)} {}

    // Takes in everything the receiver has, once per paint. A new sender starts afresh once it has
    // sent a valid message (its frames never mix with an earlier source's), so a connection that
    // sends nothing, or nothing valid, leaves the run being examined in place. The source's
    // connection and error are recorded.
    void receive(MessageReceiver& receiver, Clock::time_point now);

    // A new source has sent its first message: what came before belonged to another, so the run
    // and the point are dropped (receive() calls this; tests may call it directly).
    void begin_connection();

    // Applies one message. A message that starts a new run drops the point, whose frame belonged
    // to the old one. receive() calls this; tests may call it directly.
    void apply(const Message& message, Clock::time_point now);

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

    // The state, which tests also set directly (for example, whether a source is connected).
    [[nodiscard]] ViewerState& state() noexcept { return state_; }
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    [[nodiscard]] const ContextPublisher& publisher() const noexcept { return publisher_; }
    [[nodiscard]] std::uint64_t paints() const noexcept { return paints_; }

    // Replaces the publisher, so a test can publish somewhere writes fail.
    void set_publisher(std::filesystem::path context_file) { publisher_ = ContextPublisher{std::move(context_file)}; }

private:
    ViewerState state_;
    Selection selection_;
    ContextPublisher publisher_;
    std::optional<Clock::time_point> last_frame_at_;
    std::uint64_t paints_ = 0;
    std::vector<Message> received_;           // reused every paint, so receiving doesn't allocate
    bool connection_pending_ = false;         // a new sender hasn't yet sent a valid message
    std::optional<std::string> reported_error_;  // the publishing error last reported
};

}  // namespace fillscope
