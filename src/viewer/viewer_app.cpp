#include "viewer/viewer_app.h"

#include <format>
#include <variant>
#include <vector>

namespace fillscope {

void ViewerApp::receive(MessageReceiver& receiver, Clock::time_point now) {
    received_.clear();  // keeps its storage
    if (receiver.drain(received_).new_sender) {
        connection_pending_ = true;
    }
    for (const Message& message : received_) {
        if (connection_pending_) {
            begin_connection();
            connection_pending_ = false;
        }
        apply(message, now);
    }
    state_.source_connected = receiver.connected();
    state_.source_error = receiver.error();
}

void ViewerApp::begin_connection() {
    state_.begin_connection();
    selection_.new_run();
    last_frame_at_.reset();
}

void ViewerApp::apply(const Message& message, Clock::time_point now) {
    if (state_.apply(message)) {
        selection_.new_run();
    }
    if (std::holds_alternative<Frame>(message)) {
        last_frame_at_ = now;  // a frame, not just any message, resets "seconds since last frame"
    }
}

ViewerActions ViewerApp::paint(Clock::time_point now) {
    ViewerTiming timing{.seconds_since_frame = std::nullopt, .paints = paints_};
    if (last_frame_at_) {
        timing.seconds_since_frame = std::chrono::duration<double>(now - *last_frame_at_).count();
    }
    const ViewerActions actions = draw_viewer(ViewerView{.state = state_,
                                                         .timing = timing,
                                                         .published = publisher_.published(),
                                                         .published_bar = publisher_.published_bar(),
                                                         .publish_error = publisher_.error()});
    fillscope::apply(actions, selection_);
    publisher_.update(make_context(state_, selection_),
                      actions.any() ? PublishTrigger::user_action : PublishTrigger::background, now);
    ++paints_;
    return actions;
}

bool ViewerApp::finish() {
    publisher_.finish(make_context(state_, selection_));
    return !publisher_.error();
}

std::optional<std::string> ViewerApp::publish_report() {
    const std::optional<std::string>& error = publisher_.error();
    if (error == reported_error_) {
        return std::nullopt;
    }
    reported_error_ = error;
    if (error) {
        return std::format("can't publish the context to {}: {}", publisher_.file().string(), *error);
    }
    return std::format("publishing the context to {} again", publisher_.file().string());
}

}  // namespace fillscope
