#include "viewer/viewer_app.h"

#include <algorithm>
#include <format>
#include <utility>

namespace fillscope {

namespace {

// Makes `books` those as of update `seq` of `kept`, a moment that can be shown: forward within a run,
// a few updates, by applying them as the feed's books did; otherwise rebuilt from the nearest
// checkpoint (BookBuilder::as_of), with the histories `feed` has. Live and replay show moments this
// way. False, with the books unchanged, if `seq` isn't kept.
bool show_moment(BookBuilder& books, std::uint64_t seq, const TimeShiftBuffer& kept, const BookBuilder& feed) {
    bool stepped = books.status() == BookBuilder::Status::live && books.last_seq() <= seq &&
                   seq - books.last_seq() < TimeShiftBuffer::kCheckpointEvery;
    for (std::uint64_t next = books.last_seq() + 1; stepped && next <= seq; ++next) {
        const MarketUpdate* update = kept.update(next);
        if (update == nullptr) {
            stepped = false;  // a gap, or a run's start: rebuilt below
            break;
        }
        books.on_incremental(*update);
    }
    if (!stepped || books.last_seq() != seq) {
        const auto moment = kept.moment(seq);
        if (!moment) {
            return false;
        }
        books = BookBuilder::as_of(seq, moment->books, moment->last_trades, feed);
    }
    return true;
}

}  // namespace

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

// The feed's books take every update; the books shown take it too while they follow live, so the
// two stay the same, and stop while time-shifted.
void ViewerApp::apply_incremental(const MarketUpdate& update, Clock::time_point now) {
    kept_.received(update, now);
    state_.live.on_incremental(update);
    kept_.observe(state_.live);
    write_kept();
    if (!state_.time_shift) {
        state_.books.on_incremental(update);
    }
    last_update_at_ = now;
}

void ViewerApp::apply_snapshot(const MarketUpdate& update) {
    state_.live.on_snapshot(update);
    kept_.observe(state_.live);
    write_kept();
    if (!state_.time_shift) {
        state_.books.on_snapshot(update);
    }
}

void ViewerApp::keep_to(KeptFileWriter writer) {
    state_.kept_file = std::filesystem::absolute(writer.path());
    kept_writer_.emplace(std::move(writer));
    write_kept();
}

void ViewerApp::write_kept() {
    if (!kept_writer_) {
        return;
    }
    if (auto written = kept_writer_->write_new(kept_); !written) {
        kept_file_error_ = std::format("writing what is kept stopped: {}; live moments have no address now", written.error());
        kept_writer_.reset();
        state_.kept_file.reset();
    }
}

std::optional<std::string> ViewerApp::kept_file_report() { return std::exchange(kept_file_error_, std::nullopt); }

void ViewerApp::pause() {
    if (state_.time_shift) {
        state_.time_shift->playing = false;  // playing: stay at the update shown
        return;
    }
    // Only where the books shown are a moment kept: the newest. While the feed recovers they still
    // are (the last before the gap), so pause holds them there too.
    if (state_.replay || state_.answer || !kept_.newest() || kept_.newest() != state_.live.last_seq()) {
        return;
    }
    state_.time_shift = TimeShift{.shown = state_.live.last_seq(), .playing = false, .play_from = {}, .play_since = {},
                                  .newest = state_.live.last_seq(), .behind_seconds = 0.0};
}

void ViewerApp::play(Clock::time_point now) {
    if (!state_.time_shift || state_.time_shift->playing) {
        return;
    }
    TimeShift& shift = *state_.time_shift;
    shift.playing = true;
    shift.play_since = now;
    shift.play_from = arrival_for_play(shift.shown).value_or(now);
}

void ViewerApp::go_to(std::uint64_t seq) {
    if (!state_.time_shift) {
        pause();
    }
    if (!state_.time_shift) {
        return;
    }
    state_.time_shift->playing = false;
    if (const auto shown = kept_.nearest_shown(seq)) {
        show(*shown);
    }
}

void ViewerApp::go_live() {
    if (!state_.time_shift) {
        return;
    }
    state_.time_shift.reset();
    state_.books = state_.live.copy_keeping_no_histories();
}

std::optional<ViewerApp::Clock::time_point> ViewerApp::arrival_for_play(std::uint64_t seq) const {
    if (const auto arrived = kept_.arrived(seq)) {
        return arrived;
    }
    if (const auto next = kept_.next(seq)) {
        return kept_.arrived(*next);
    }
    return std::nullopt;
}

void ViewerApp::show(std::uint64_t seq) {
    if (show_moment(state_.books, seq, kept_, state_.live)) {  // always: seq is one that can be shown
        state_.time_shift->shown = seq;
    }
}

void ViewerApp::play_replay(Clock::time_point now) {
    if (!state_.replay) {
        return;
    }
    if (state_.books.last_seq() >= state_.replay->updates.size()) {
        replay_to(0);  // at the end: from the start again
    }
    ReplayPlay& play = state_.replay->play;
    play.playing = true;
    play.from = state_.books.last_seq();
    play.since = now;
}

void ViewerApp::pause_replay() {
    if (state_.replay) {
        state_.replay->play.playing = false;
    }
}

void ViewerApp::set_replay_rate(double rate, Clock::time_point now) {
    if (!state_.replay || !(rate > 0)) {
        return;
    }
    ReplayPlay& play = state_.replay->play;
    play.rate = rate;
    play.from = state_.books.last_seq();  // on from here, at the new rate
    play.since = now;
}

void ViewerApp::advance(Clock::time_point now) {
    if (state_.replay && state_.replay->play.playing) {
        ReplayPlay& play = state_.replay->play;
        const std::uint64_t last = state_.replay->updates.size();
        const double elapsed = std::chrono::duration<double>(now - play.since).count();
        std::uint64_t target = play.from + static_cast<std::uint64_t>(std::max(elapsed, 0.0) * play.rate);
        if (target >= last) {
            target = last;
            play.playing = false;
        }
        if (target != state_.books.last_seq()) {
            replay_to(target);
        }
    }
    if (!state_.time_shift) {
        return;
    }
    TimeShift& shift = *state_.time_shift;
    if (shift.playing) {
        const Clock::time_point clock = shift.play_from + (now - shift.play_since);
        std::uint64_t target = shift.shown;
        for (auto next = kept_.next(target); next; next = kept_.next(*next)) {
            const auto arrived = arrival_for_play(*next);
            if (!arrived || *arrived > clock) {
                break;
            }
            target = *next;
        }
        if (target != shift.shown) {
            show(target);
        }
    }
    shift.newest = kept_.newest().value_or(shift.shown);
    const auto newest_arrived = arrival_for_play(shift.newest);
    const auto shown_arrived = arrival_for_play(shift.shown);
    shift.behind_seconds = newest_arrived && shown_arrived
                               ? std::optional{std::chrono::duration<double>(*newest_arrived - *shown_arrived).count()}
                               : std::nullopt;
}

void ViewerApp::open_replay(const std::filesystem::path& file, MarketDataFile market_data) {
    state_.books = BookBuilder::keeping_no_histories();
    state_.replay = Replay::of(std::filesystem::absolute(file), std::move(market_data.recording), std::move(market_data.updates));
}

void ViewerApp::open_session(const std::filesystem::path& file, MarketDataFile market_data, SessionTruth truth) {
    open_replay(file, std::move(market_data));
    state_.replay->truth = std::move(truth);
}

void ViewerApp::replay_to(std::uint64_t seq) {
    if (!state_.replay) {
        return;
    }
    const Replay& replay = *state_.replay;
    seq = std::min<std::uint64_t>(seq, replay.updates.size());
    if (seq == 0) {
        state_.books = BookBuilder::keeping_no_histories();  // before the first update: waiting for it
        return;
    }
    // What is kept ends before an update the books can't apply, if one comes; past it, the books are
    // the last kept moment's, given that update, and recovering.
    const std::uint64_t last_kept = replay.kept.newest().value_or(0);
    if (last_kept == 0) {
        state_.books = BookBuilder::keeping_no_histories();  // update 1 is the one it can't apply
    } else {
        (void)show_moment(state_.books, std::min(seq, last_kept), replay.kept, replay.books);  // always kept
    }
    if (seq > last_kept) {
        state_.books.on_incremental(replay.updates[last_kept]);
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
        state_.books = BookBuilder::keeping_no_histories();
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
    advance(now);
    state_.books_kept = kept_.updates_to_apply(state_.books.last_seq()).has_value();
    ViewerTiming timing{.seconds_since_update = std::nullopt};
    if (last_update_at_) {
        timing.seconds_since_update = std::chrono::duration<double>(now - *last_update_at_).count();
    }
    const ViewerActions actions = draw_viewer(ViewerView{.state = state_,
                                                         .timing = timing,
                                                         .published = publisher_.published(),
                                                         .published_bar = publisher_.published_bar(),
                                                         .publish_error = publisher_.error(),
                                                         .kept = state_.replay || state_.answer ? nullptr : &kept_});
    if (actions.pause) {
        pause();
    }
    if (actions.go_to) {
        go_to(*actions.go_to);
    }
    if (actions.play) {
        play(now);
    }
    if (actions.go_live) {
        go_live();
    }
    if (actions.pause || actions.go_to || actions.play || actions.go_live) {
        advance(now);  // how far behind, as of now, in this paint's context
    }
    if (actions.replay_to) {
        pause_replay();  // a move of the user's own takes over from playing
        replay_to(*actions.replay_to);
    }
    if (actions.replay_pause) {
        pause_replay();
    }
    if (actions.replay_rate) {
        set_replay_rate(*actions.replay_rate, now);
    }
    if (actions.replay_play) {
        play_replay(now);
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
    // The file is deleted as the viewer exits: the last context gives no address in it.
    state_.kept_file.reset();
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
