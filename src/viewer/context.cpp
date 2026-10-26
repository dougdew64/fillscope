#include "viewer/context.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <span>

namespace fillscope {

void Selection::point_at(const PointTarget& target) {
    pointed_ = target;
    point_seq_ = next_seq_++;
}

void Selection::clear_point() {
    pointed_.reset();
    point_seq_ = next_seq_++;
}

void Selection::follow(const OrderRef& order) {
    followed_ = order;
    follow_seq_ = next_seq_++;
}

void Selection::clear_follow() {
    followed_.reset();
    follow_seq_ = next_seq_++;
}

OrderState order_state(const BookBuilder& books, const OrderRef& ref) {
    OrderState state;
    state.ref = ref;
    if (ref.ticker >= BookBuilder::kTickers) {
        return state;
    }
    const Book& book = books.book(ref.ticker);
    if (const Order* order = book.find(ref.order_id)) {
        state.resting = *order;
        // Its place in its price's queue: the orders that fill before it.
        if (const auto level = book.level(order->side, order->price)) {
            for (const Order& other : level->orders) {
                if (other.order_id == ref.order_id) {
                    break;
                }
                ++state.orders_ahead;
                state.qty_ahead += other.qty;
            }
        }
    }
    const auto history = books.history(ref.ticker, ref.order_id);
    if (!history.empty()) {
        state.last_event = history.back();
        if (!state.resting && state.last_event->type != UpdateType::cancel) {
            // It left without a cancel the books saw: in the first recovery covering updates after
            // its last one.
            for (const Recovery& recovery : books.recoveries()) {
                if (recovery.snapshot_seq > state.last_event->seq) {
                    state.left_during = recovery;
                    break;
                }
            }
        }
    }
    return state;
}

namespace {

LevelState level_state(const BookBuilder& books, const LevelRef& ref) {
    LevelState state;
    state.ref = ref;
    if (ref.ticker >= BookBuilder::kTickers) {
        return state;
    }
    if (const auto level = books.book(ref.ticker).level(ref.side, ref.price)) {
        state.present = true;
        state.qty = level->qty;
        state.order_count = level->orders.size();
        const auto listed = std::min(level->orders.size(), kMaxLevelOrders);
        state.orders.assign(level->orders.begin(), level->orders.begin() + static_cast<std::ptrdiff_t>(listed));
    }
    return state;
}

}  // namespace

Context make_context(const ViewerState& state, const Selection& selection) {
    // The books shown, which what is pointed at and followed are in; and the feed's, live, whose
    // health is the background's (in a replay or an answer, the books shown).
    const BookBuilder& books = state.books;
    const BookBuilder& feed_books = state.replay || state.answer ? state.books : state.live;
    Context context;
    Context::Feed& feed = context.feed;
    feed.status = feed_books.status();
    feed.last_seq = feed_books.last_seq();
    feed.books_seq = books.last_seq();
    feed.updates_received = feed_books.updates_received();
    feed.recovering_because = feed_books.recovering_because();
    feed.recovery_count = feed_books.recoveries().size();
    if (state.time_shift) {
        feed.time_shift = Context::Feed::TimeShifted{.shown_seq = state.time_shift->shown,
                                                     .newest_seq = state.time_shift->newest,
                                                     .behind_seconds = state.time_shift->behind_seconds,
                                                     .playing = state.time_shift->playing};
    }
    const auto recoveries = feed_books.recoveries();
    const auto latest = recoveries.last(std::min(recoveries.size(), kMaxListedRecoveries));
    feed.latest_recoveries.assign(latest.begin(), latest.end());
    feed.error = state.feed_error;
    feed.shown_ticker = state.shown_ticker;
    // In a replay, the books are exactly the recorded run's after update last_seq: an address for it.
    std::optional<ReplayAddress> here;
    if (state.replay) {
        feed.replay = ReplayAddress{.market_data_file = state.replay->file.string(),
                                    .recording = state.replay->recording,
                                    .seq = state.replay->updates.size()};
        here = ReplayAddress{.market_data_file = feed.replay->market_data_file, .recording = feed.replay->recording,
                             .seq = books.last_seq()};
    }
    if (state.course && !state.course->chosen) {
        feed.no_answer = "nothing chosen yet: neither a lesson nor the user's latest question";
    } else if (state.answer && !state.answer->answer) {
        feed.no_answer = state.answer->problem ? std::format("the answer at {} can't be shown: {}", state.answer->file.string(),
                                                             *state.answer->problem)
                                               : std::format("waiting for an answer at {}", state.answer->file.string());
    }
    if (state.answer && state.answer->answer) {
        const LoadedAnswer& answer = *state.answer->answer;
        const AnswerFrame& frame = answer.frames.at(state.answer->frame);
        feed.answer = AnswerPlace{.file = state.answer->file.string(),
                                  .question = answer.scenario.question,
                                  .step = frame.step,
                                  .steps = answer.scenario.steps.size(),
                                  .asking = frame.asking,
                                  .lesson = {}};
        if (state.course && state.course->chosen > 0) {
            feed.answer->lesson = std::format("lesson {}, \"{}\"", *state.course->chosen,
                                              state.course->course.lessons.at(*state.course->chosen - 1).title);
        }
    }

    if (const auto& pointed = selection.pointed()) {
        context.point = Context::Point{.seq = selection.point_seq(), .target = OrderState{}, .replay = here};
        if (const auto* order = std::get_if<OrderRef>(&*pointed)) {
            context.point->target = order_state(books, *order);
        } else {
            context.point->target = level_state(books, std::get<LevelRef>(*pointed));
        }
    }

    if (const auto& followed = selection.followed()) {
        Context::Follow follow;
        follow.seq = selection.follow_seq();
        follow.order = order_state(books, *followed);
        follow.replay = here;
        if (followed->ticker < BookBuilder::kTickers) {
            const auto history = books.history(followed->ticker, followed->order_id);
            follow.event_count = history.size();
            const auto listed = history.first(std::min(history.size(), kMaxFollowedEvents));
            follow.events.assign(listed.begin(), listed.end());
            if (!history.empty()) {
                for (const Recovery& recovery : books.recoveries()) {
                    if (recovery.snapshot_seq >= history.front().seq) {
                        follow.gaps.push_back(recovery);
                    }
                }
            }
        }
        context.follow = std::move(follow);
    }
    return context;
}

namespace {

// A std::optional as JSON: its value, or null when it is empty.
template <typename T>
nlohmann::ordered_json or_null(const std::optional<T>& value) {
    return value ? nlohmann::ordered_json(*value) : nlohmann::ordered_json(nullptr);
}

std::string_view status_name(BookBuilder::Status status) {
    switch (status) {
    case BookBuilder::Status::waiting: return "waiting";
    case BookBuilder::Status::live: return "live";
    case BookBuilder::Status::recovering: return "recovering";
    }
    return "unknown";
}

nlohmann::ordered_json event_json(const OrderEvent& event) {
    return {{"seq", event.seq},           {"type", to_string(event.type)}, {"side", to_string(event.side)},
            {"price", event.price},       {"qty", event.qty},              {"fromSnapshot", event.from_snapshot}};
}

nlohmann::ordered_json replay_json(const std::optional<ReplayAddress>& address) {
    if (!address) {
        return nullptr;
    }
    return {{"marketDataFile", address->market_data_file}, {"recording", address->recording}, {"seq", address->seq}};
}

nlohmann::ordered_json recovery_json(const Recovery& recovery) {
    return {{"afterSeq", recovery.after_seq}, {"snapshotSeq", recovery.snapshot_seq}, {"reason", recovery.reason}};
}

nlohmann::ordered_json order_json(const OrderState& order) {
    nlohmann::ordered_json resting = nullptr;
    if (order.resting) {
        resting = {{"side", to_string(order.resting->side)},
                   {"price", order.resting->price},
                   {"qty", order.resting->qty},
                   {"priority", order.resting->priority},
                   {"ordersAhead", order.orders_ahead},
                   {"qtyAhead", order.qty_ahead}};
    }
    nlohmann::ordered_json last_event = nullptr;
    if (order.last_event) {
        last_event = event_json(*order.last_event);
    }
    nlohmann::ordered_json left_during = nullptr;
    if (order.left_during) {
        left_during = recovery_json(*order.left_during);
    }
    return {{"ticker", order.ref.ticker}, {"orderId", order.ref.order_id}, {"resting", resting}, {"lastEvent", last_event},
            {"leftDuringRecovery", left_during}};
}

nlohmann::ordered_json level_json(const LevelState& level) {
    nlohmann::ordered_json orders = nlohmann::ordered_json::array();
    for (const Order& order : level.orders) {
        orders.push_back({{"orderId", order.order_id}, {"qty", order.qty}, {"priority", order.priority}});
    }
    return {{"ticker", level.ref.ticker},  {"side", to_string(level.ref.side)}, {"price", level.ref.price},
            {"present", level.present},    {"qty", level.qty},                    {"orderCount", level.order_count},
            {"orders", orders},            {"ordersTruncated", level.truncated()}};
}

}  // namespace

nlohmann::ordered_json to_json(const Context& context, const Writer& writer, std::uint64_t seq,
                               std::int64_t written_at_ms) {
    nlohmann::ordered_json json;
    json["version"] = 2;
    json["seq"] = seq;
    json["writtenAtMs"] = written_at_ms;
    json["writer"] = {{"program", "viewer"}, {"pid", writer.pid}, {"startedAtMs", writer.started_at_ms},
                      {"running", writer.running}};
    json["about"] =
        "What the Fillscope viewer's user has pointed at and followed in the exchange's order books, which the "
        "viewer rebuilds from the exchange's market data. 'point' is an order or a price level, the subject; "
        "'follow' is an order through every update the books applied to it. Both are as of the incremental "
        "update their asOfSeq names: background.feed.lastSeq, unless time-shifted (background.timeShift.shownSeq). A resting order's ordersAhead and qtyAhead are what fills before it at "
        "its price. Each section's seq is taken when the user changes that section, so the higher one is what "
        "the user did last. While background.feed.status is 'recovering', updates were missed and the books are "
        "as of lastSeq, older than the exchange; follow.gaps lists the recoveries whose updates an order's "
        "events may be missing. In a replay (background.replay), the books are exactly a recorded run's after "
        "update lastSeq, and point and follow carry a replayAddress that rebuilds that state: apply the market data "
        "file's updates 1 to seq. In an answer (background.answer), the user is at that step of it, 0 being its "
        "intro; while 'asking', its question is shown and the step not yet played, so the books are as before it. "
        "A viewer showing answers with none to show says why in background.noAnswer. An answer that is a lesson of "
        "the course names it in background.answer.lesson. "
        "Live, while the viewer shows a moment behind the newest update (paused, or playing behind live, as a "
        "television recorder does), background.timeShift says which update is shown (shownSeq), the newest "
        "(newestSeq) and how far behind in seconds of the viewer's own arrival times (behindSeconds); then point and "
        "follow are as of shownSeq, and background.feed is the live feed's. "
        "'writer' says which viewer wrote this file and whether it is still running.";

    const Context::Feed& feed = context.feed;
    nlohmann::ordered_json latest = nlohmann::ordered_json::array();
    for (const Recovery& recovery : feed.latest_recoveries) {
        latest.push_back(recovery_json(recovery));
    }
    json["background"] = {
        {"feed",
         {{"status", status_name(feed.status)},
          {"lastSeq", feed.last_seq},
          {"updatesReceived", feed.updates_received},
          {"recoveringBecause", or_null(feed.recovering_because)},
          {"recoveryCount", feed.recovery_count},
          {"latestRecoveries", latest},
          {"error", or_null(feed.error)}}},
        {"shownTicker", feed.shown_ticker},
        {"replay", feed.replay ? nlohmann::ordered_json{{"marketDataFile", feed.replay->market_data_file},
                                                        {"recording", feed.replay->recording},
                                                        {"updates", feed.replay->seq}}
                               : nlohmann::ordered_json(nullptr)},
        {"noReplayAddress", feed.replay ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(kNoReplayAddress)},
        {"answer", feed.answer ? nlohmann::ordered_json{{"file", feed.answer->file},
                                                        {"question", feed.answer->question},
                                                        {"step", feed.answer->step},
                                                        {"steps", feed.answer->steps},
                                                        {"asking", feed.answer->asking},
                                                        {"lesson", feed.answer->lesson.empty() ? nlohmann::ordered_json(nullptr)
                                                                                                : nlohmann::ordered_json(feed.answer->lesson)}}
                               : nlohmann::ordered_json(nullptr)},
        {"noAnswer", or_null(feed.no_answer)},
        {"timeShift", feed.time_shift ? nlohmann::ordered_json{{"shownSeq", feed.time_shift->shown_seq},
                                                               {"newestSeq", feed.time_shift->newest_seq},
                                                               {"behindUpdates", feed.time_shift->newest_seq - feed.time_shift->shown_seq},
                                                               {"behindSeconds", or_null(feed.time_shift->behind_seconds)},
                                                               {"playing", feed.time_shift->playing}}
                                      : nlohmann::ordered_json(nullptr)},
    };

    if (context.point) {
        const Context::Point& point = *context.point;
        json["point"] = {{"seq", point.seq}, {"asOfSeq", feed.books_seq}, {"replayAddress", replay_json(point.replay)}};
        if (const auto* order = std::get_if<OrderState>(&point.target)) {
            json["point"]["order"] = order_json(*order);
        } else {
            json["point"]["level"] = level_json(std::get<LevelState>(point.target));
        }
    } else {
        json["point"] = nullptr;
    }

    if (context.follow) {
        const Context::Follow& follow = *context.follow;
        nlohmann::ordered_json events = nlohmann::ordered_json::array();
        for (const OrderEvent& event : follow.events) {
            events.push_back(event_json(event));
        }
        nlohmann::ordered_json gaps = nlohmann::ordered_json::array();
        for (const Recovery& gap : follow.gaps) {
            gaps.push_back(recovery_json(gap));
        }
        json["follow"] = {{"seq", follow.seq},
                          {"asOfSeq", feed.books_seq},
                          {"order", order_json(follow.order)},
                          {"eventCount", follow.event_count},
                          {"events", events},
                          {"eventsTruncated", follow.truncated()},
                          {"gaps", gaps},
                          {"replayAddress", replay_json(follow.replay)}};
    } else {
        json["follow"] = nullptr;
    }
    return json;
}

namespace {

std::string plural(std::size_t count, std::string_view one, std::string_view many) {
    return std::format("{} {}", count, count == 1 ? one : many);
}

// How an order stands, for the bar: resting, with its place; or how it left; or never seen.
std::string order_summary(const OrderState& order) {
    if (order.resting) {
        return std::format("{} {} @ {}, {} ({}) ahead of it in its queue", to_string(order.resting->side), order.resting->qty,
                           order.resting->price, plural(order.orders_ahead, "order", "orders"), order.qty_ahead);
    }
    if (order.left_during) {
        return std::format("not in the book; it left during a recovery (updates {} to {} weren't seen one by one), after its "
                           "last update seen: {} at update {}",
                           order.left_during->after_seq + 1, order.left_during->snapshot_seq, to_string(order.last_event->type),
                           order.last_event->seq);
    }
    if (order.last_event) {
        return std::format("not in the book (its last update: {} at update {})", to_string(order.last_event->type),
                           order.last_event->seq);
    }
    return "never seen";
}

}  // namespace

ContextBarText bar_text(const Context& context) {
    ContextBarText text;
    const Context::Feed& feed = context.feed;
    if (feed.no_answer) {
        text.background = std::format("no answer shown: {}", *feed.no_answer);
    } else if (feed.answer) {
        const AnswerPlace& answer = *feed.answer;
        const std::string what = answer.lesson.empty() ? std::format("answer to \"{}\"", answer.question) : answer.lesson;
        text.background = answer.step == 0 ? std::format("{}: its introduction", what)
                                           : std::format("{}: step {} of {}{}", what, answer.step, answer.steps,
                                                         answer.asking ? ", its question (not yet played)" : "");
    } else if (feed.replay) {
        text.background = std::format("replay of {} at update {} of {}", feed.replay->market_data_file, feed.last_seq,
                                      feed.replay->seq);
        if (feed.status == BookBuilder::Status::recovering) {
            text.background += std::format(", stuck: {}", feed.recovering_because.value_or("an update can't be applied"));
        }
    } else {
        switch (feed.status) {
        case BookBuilder::Status::waiting:
            text.background = "waiting for the exchange's market data";
            break;
        case BookBuilder::Status::live:
            text.background = std::format("exchange live at update {}", feed.last_seq);
            break;
        case BookBuilder::Status::recovering:
            text.background = std::format("exchange: recovering, the books are as of update {} ({})", feed.last_seq,
                                          feed.recovering_because.value_or("updates were missed"));
            break;
        }
    }
    // Each line begins with a capital, as the pointing and following lines do; a lesson's title has one.
    text.background[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text.background[0])));
    if (feed.time_shift) {
        const auto& shift = *feed.time_shift;
        const std::uint64_t behind = shift.newest_seq - shift.shown_seq;
        text.background += std::format(" · {} at update {}, {} behind{}", shift.playing ? "playing" : "paused", shift.shown_seq,
                                       plural(behind, "update", "updates"),
                                       shift.behind_seconds ? std::format(" ({:.1f} s)", *shift.behind_seconds) : std::string{});
    }
    text.background += std::format(" · showing ticker {}", feed.shown_ticker);
    if (feed.recovery_count > 0) {
        text.background += std::format(" · {}", plural(feed.recovery_count, "recovery", "recoveries"));
    }
    if (feed.error) {
        text.background += std::format(" · last error: {}", *feed.error);
    }

    if (context.point) {
        if (const auto* order = std::get_if<OrderState>(&context.point->target)) {
            text.point = std::format("Pointing at order {} of ticker {}: {}", order->ref.order_id, order->ref.ticker,
                                     order_summary(*order));
        } else {
            const LevelState& level = std::get<LevelState>(context.point->target);
            text.point = level.present
                             ? std::format("Pointing at {} {} of ticker {}: {} in {}", to_string(level.ref.side), level.ref.price,
                                           level.ref.ticker, level.qty, plural(level.order_count, "order", "orders"))
                             : std::format("Pointing at {} {} of ticker {}: no orders at that price now",
                                           to_string(level.ref.side), level.ref.price, level.ref.ticker);
        }
    }
    if (context.follow) {
        const Context::Follow& follow = *context.follow;
        text.follow = std::format("Following order {} of ticker {}: {}{}, {}", follow.order.ref.order_id, follow.order.ref.ticker,
                                  plural(follow.event_count, "event", "events"),
                                  follow.truncated() ? std::format(" (first {} listed)", follow.events.size()) : std::string{},
                                  order_summary(follow.order));
        if (!follow.gaps.empty()) {
            *text.follow += std::format(" · {} may hide some of its events", plural(follow.gaps.size(), "recovery", "recoveries"));
        }
    }
    return text;
}

}  // namespace fillscope
