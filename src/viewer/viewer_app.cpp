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
    kept_.received(update, now);
    state_.books.on_incremental(update);
    kept_.observe(state_.books);
    last_update_at_ = now;
}

void ViewerApp::apply_snapshot(const MarketUpdate& update) {
    state_.books.on_snapshot(update);
    kept_.observe(state_.books);
}

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

void ViewerApp::watch_answer(const std::filesystem::path& file) {
    state_.answer = AnswerShown{.file = std::filesystem::absolute(file), .stamp = std::nullopt, .answer = std::nullopt,
                                .problem = std::nullopt, .frame = 0};
    answer_polled_at_.reset();
}

void ViewerApp::poll_answer(Clock::time_point now) {
    if (!state_.answer || (state_.course && !state_.course->chosen) || (answer_polled_at_ && now - *answer_polled_at_ < kAnswerPollInterval)) {
        return;
    }
    answer_polled_at_ = now;
    AnswerShown& shown = *state_.answer;
    const std::optional<FileStamp> stamp = stamp_of(shown.file);
    if (stamp == shown.stamp) {
        return;  // the version already read, or still no file
    }
    shown.stamp = stamp;
    auto loaded = load_answer(shown.file);
    shown.frame = 0;
    if (!loaded) {
        // Nothing of an earlier answer stays on screen: its books would be taken for this one's.
        shown.answer.reset();
        shown.problem = std::move(loaded.error());
        state_.replay.reset();
        state_.books = BookBuilder{};
        return;
    }
    shown.problem.reset();
    open_replay(loaded->market_data_file, loaded->market_data);
    state_.shown_ticker = loaded->scenario.ticker;
    shown.answer = std::move(*loaded);
    answer_to(0);
}

void ViewerApp::answer_to(std::size_t frame) {
    if (!state_.answer || !state_.answer->answer) {
        return;
    }
    AnswerShown& shown = *state_.answer;
    shown.frame = std::min(frame, shown.answer->frames.size() - 1);
    replay_to(shown.answer->frames[shown.frame].books_after);
}

void ViewerApp::open_course(Course course, const std::filesystem::path& built) {
    if (!state_.answer) {
        return;
    }
    state_.course = CourseChoice{.course = std::move(course), .built = std::filesystem::absolute(built),
                                 .questions = state_.answer->file, .chosen = std::nullopt};
}

void ViewerApp::choose(std::size_t choice) {
    if (!state_.course) {
        return;
    }
    state_.course->chosen = std::min(choice, state_.course->course.lessons.size());
    watch_answer(state_.course->chosen_file());  // read at the next poll, which is at once
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
    if (actions.choose) {
        choose(*actions.choose);
        poll_answer(now);  // the chosen answer shows in this paint's context, not a paint later
    }
    if (actions.answer_to) {
        answer_to(*actions.answer_to);
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
