#include "viewer/viewer_app.h"

#include <variant>
#include <vector>

namespace fillscope {

void ViewerApp::receive(MessageReceiver& receiver, Clock::time_point now) {
    std::vector<Message> received;
    if (receiver.drain(received).new_sender) {
        begin_connection();
    }
    for (const Message& message : received) {
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
    const ViewerActions actions = draw_viewer(ViewerView{
        .state = state_, .timing = timing, .published = publisher_.published(), .publish_error = publisher_.error()});
    fillscope::apply(actions, selection_);
    publisher_.update(make_context(state_, selection_),
                      actions.any() ? PublishTrigger::user_action : PublishTrigger::background, now);
    ++paints_;
    return actions;
}

void ViewerApp::finish() { publisher_.finish(make_context(state_, selection_)); }

}  // namespace fillscope
