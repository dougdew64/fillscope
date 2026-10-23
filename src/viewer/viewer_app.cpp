#include "viewer/viewer_app.h"

#include <format>

namespace fillscope {

void ViewerApp::receive(FeedSocket& incremental, FeedSocket& snapshot, Clock::time_point now) {
    received_.clear();  // keeps its storage
    if (const auto result = incremental.receive(received_, kMaxDatagramsPerPaint); result.error) {
        state_.feed_error = result.error;
    }
    for (const MarketUpdate& update : received_) {
        apply_incremental(update, now);
    }
    received_.clear();
    if (const auto result = snapshot.receive(received_, kMaxDatagramsPerPaint); result.error) {
        state_.feed_error = result.error;
    }
    for (const MarketUpdate& update : received_) {
        apply_snapshot(update);
    }
}

void ViewerApp::apply_incremental(const MarketUpdate& update, Clock::time_point now) {
    state_.books.on_incremental(update);
    last_update_at_ = now;
}

void ViewerApp::apply_snapshot(const MarketUpdate& update) { state_.books.on_snapshot(update); }

ViewerActions ViewerApp::paint(Clock::time_point now) {
    ViewerTiming timing{.seconds_since_update = std::nullopt, .paints = paints_};
    if (last_update_at_) {
        timing.seconds_since_update = std::chrono::duration<double>(now - *last_update_at_).count();
    }
    const ViewerActions actions = draw_viewer(ViewerView{.state = state_,
                                                         .timing = timing,
                                                         .published = publisher_.published(),
                                                         .published_bar = publisher_.published_bar(),
                                                         .publish_error = publisher_.error()});
    fillscope::apply(actions, selection_, state_);
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
