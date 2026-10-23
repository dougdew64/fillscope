#include "viewer/context.h"

#include <algorithm>
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
    const BookBuilder& books = state.books;
    Context context;
    Context::Feed& feed = context.feed;
    feed.status = books.status();
    feed.last_seq = books.last_seq();
    feed.updates_received = books.updates_received();
    feed.recovering_because = books.recovering_because();
    feed.recovery_count = books.recoveries().size();
    const auto recoveries = books.recoveries();
    const auto latest = recoveries.last(std::min(recoveries.size(), kMaxListedRecoveries));
    feed.latest_recoveries.assign(latest.begin(), latest.end());
    feed.error = state.feed_error;
    feed.shown_ticker = state.shown_ticker;

    if (const auto& pointed = selection.pointed()) {
        context.point = Context::Point{.seq = selection.point_seq(), .target = OrderState{}};
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
    return {{"ticker", order.ref.ticker}, {"orderId", order.ref.order_id}, {"resting", resting}, {"lastEvent", last_event}};
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
        "update background.feed.lastSeq. A resting order's ordersAhead and qtyAhead are what fills before it at "
        "its price. Each section's seq is taken when the user changes that section, so the higher one is what "
        "the user did last. While background.feed.status is 'recovering', updates were missed and the books are "
        "as of lastSeq, older than the exchange; follow.gaps lists the recoveries whose updates an order's "
        "events may be missing. 'writer' says which viewer wrote this file and whether it is still running.";

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
        {"noReplayAddress", kNoReplayAddress},
    };

    if (context.point) {
        const Context::Point& point = *context.point;
        json["point"] = {{"seq", point.seq}, {"asOfSeq", feed.last_seq}};
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
                          {"asOfSeq", feed.last_seq},
                          {"order", order_json(follow.order)},
                          {"eventCount", follow.event_count},
                          {"events", events},
                          {"eventsTruncated", follow.truncated()},
                          {"gaps", gaps}};
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
