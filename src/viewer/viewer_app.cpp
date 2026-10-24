#include "viewer/viewer_app.h"

#include <algorithm>
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

void ViewerApp::open_replay(const std::filesystem::path& file, MarketDataFile market_data) {
    state_.books = BookBuilder{};
    state_.replay = Replay{.file = std::filesystem::absolute(file),
                           .recording = std::move(market_data.recording),
                           .updates = std::move(market_data.updates)};
}

void ViewerApp::replay_to(std::uint64_t seq) {
    if (!state_.replay) {
        return;
    }
    const std::vector<MarketUpdate>& updates = state_.replay->updates;
    seq = std::min<std::uint64_t>(seq, updates.size());
    if (seq < state_.books.last_seq()) {
        state_.books = BookBuilder{};
    }
    // The replay's updates are numbered 1, 2, 3, ... (read_market_data_file checks), so update n is
    // updates[n - 1]. An update the books can't apply leaves them recovering, which a replay can't do
    // (it has no snapshots): they stay where they are.
    for (std::uint64_t next = state_.books.last_seq() + 1;
         next <= seq && state_.books.status() != BookBuilder::Status::recovering; ++next) {
        state_.books.on_incremental(updates[next - 1]);
    }
}

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
    if (actions.replay_to) {
        replay_to(*actions.replay_to);
    }
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
