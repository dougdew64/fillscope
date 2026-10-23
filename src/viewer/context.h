// What the viewer's user has pointed at and followed, and the context published from it for the
// assistant working beside the viewer (docs/DECISIONS.md, 2026-10-19).
//
// The user assembles the subject of a question by pointing and clicking; the question itself is
// asked of the assistant, in another program. So the viewer can't wait to see the question: it
// publishes the assembled context as soon as it changes (context_publisher.h). Two ways to assemble
// it, kept apart because they mean different things:
//   - point at: an order, or a price level, the subject of a question ("why didn't this fill?");
//   - follow:   an order through every update the books applied to it, a lens on the question.
// Each has its own sequence number, taken only when the user changes it, so the assistant can tell
// which the user did last.
#pragma once

#include "feed/book.h"
#include "feed/book_builder.h"
#include "feed/market_update.h"
#include "viewer/viewer_state.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fillscope {

// An order, by the exchange's id within its ticker.
struct OrderRef {
    std::uint32_t ticker = 0;
    std::uint64_t order_id = 0;
    friend bool operator==(const OrderRef&, const OrderRef&) = default;
};

// A price on one side of a ticker's book.
struct LevelRef {
    std::uint32_t ticker = 0;
    Side side = Side::none;
    std::int64_t price = 0;
    friend bool operator==(const LevelRef&, const LevelRef&) = default;
};

using PointTarget = std::variant<OrderRef, LevelRef>;

// The user's choices: what is pointed at and what is followed. Changed only through the member
// functions, which keep the sequence numbers right.
class Selection {
public:
    void point_at(const PointTarget& target);
    void clear_point();
    void follow(const OrderRef& order);
    void clear_follow();

    [[nodiscard]] const std::optional<PointTarget>& pointed() const noexcept { return pointed_; }
    [[nodiscard]] const std::optional<OrderRef>& followed() const noexcept { return followed_; }
    // When each last changed, on one shared counter, so the two can be compared.
    [[nodiscard]] std::uint64_t point_seq() const noexcept { return point_seq_; }
    [[nodiscard]] std::uint64_t follow_seq() const noexcept { return follow_seq_; }

private:
    std::optional<PointTarget> pointed_;
    std::optional<OrderRef> followed_;
    std::uint64_t point_seq_ = 0;
    std::uint64_t follow_seq_ = 0;
    std::uint64_t next_seq_ = 1;
};

// At most this many orders are listed for a level pointed at, and events for an order followed; the
// counts are always exact.
inline constexpr std::size_t kMaxLevelOrders = 100;
inline constexpr std::size_t kMaxFollowedEvents = 1000;
// At most this many of the latest recoveries are listed in the background; the count is exact.
inline constexpr std::size_t kMaxListedRecoveries = 10;

// Why nothing published has a replay address yet: the exchange's runs aren't recorded.
inline constexpr std::string_view kNoReplayAddress =
    "the exchange's runs aren't recorded, so no state can be rebuilt exactly yet (deterministic replay is "
    "step 5 of docs/PLAN.md)";

// An order as the books have it now: resting, with its place in its price's queue; or not resting,
// with the last update applied to it (how it left), or never seen.
struct OrderState {
    OrderRef ref;
    std::optional<Order> resting;
    std::size_t orders_ahead = 0;     // when resting: the orders before it in its price's queue
    std::uint64_t qty_ahead = 0;      // and their total quantity
    std::optional<OrderEvent> last_event;  // empty if the books never applied an update to it

    friend bool operator==(const OrderState&, const OrderState&) = default;
};

// A price level as the books have it now; `present` is false once it has no orders.
struct LevelState {
    LevelRef ref;
    bool present = false;
    std::uint64_t qty = 0;
    std::size_t order_count = 0;
    std::vector<Order> orders;  // in the order they will fill, up to kMaxLevelOrders

    [[nodiscard]] bool truncated() const noexcept { return order_count > orders.size(); }
    friend bool operator==(const LevelState&, const LevelState&) = default;
};

// Everything published at one moment: a value, built from the viewer's state and the selection by
// make_context. The context bar draws the Context that was last published, and the file holds the
// same Context as JSON, so the bar can't show anything the file doesn't say.
struct Context {
    struct Feed {
        BookBuilder::Status status = BookBuilder::Status::waiting;
        std::uint64_t last_seq = 0;  // the books are as of this incremental update
        std::uint64_t updates_received = 0;
        std::optional<std::string> recovering_because;
        std::size_t recovery_count = 0;
        std::vector<Recovery> latest_recoveries;  // up to kMaxListedRecoveries, oldest first
        std::optional<std::string> error;        // the last datagram that couldn't be received or decoded
        std::uint32_t shown_ticker = 0;

        friend bool operator==(const Feed&, const Feed&) = default;
    };
    struct Point {
        std::uint64_t seq = 0;
        std::variant<OrderState, LevelState> target;

        friend bool operator==(const Point&, const Point&) = default;
    };
    struct Follow {
        std::uint64_t seq = 0;
        OrderState order;
        std::vector<OrderEvent> events;  // up to kMaxFollowedEvents, in order
        std::size_t event_count = 0;
        // Recoveries since the order was first seen: their updates were never applied one by one, so
        // the order's events from them are missing.
        std::vector<Recovery> gaps;

        [[nodiscard]] bool truncated() const noexcept { return event_count > events.size(); }
        friend bool operator==(const Follow&, const Follow&) = default;
    };

    Feed feed;
    std::optional<Point> point;
    std::optional<Follow> follow;

    friend bool operator==(const Context&, const Context&) = default;
};

[[nodiscard]] Context make_context(const ViewerState& state, const Selection& selection);

// An order as the books have it now (also used to draw it).
[[nodiscard]] OrderState order_state(const BookBuilder& books, const OrderRef& ref);

// Who wrote the file. The file outlives the viewer, so a reader needs to tell a current file from
// one a viewer left behind: `running` is false in the last file a viewer writes as it exits
// (including when it is stopped by SIGINT or SIGTERM), and `pid` names the process (if it is still
// running, /proc/<pid> exists). A viewer that crashes or is killed (SIGKILL) can't write its last
// file, so `running: true` with a pid that no longer exists means one of those happened.
struct Writer {
    std::int64_t pid = 0;
    std::int64_t started_at_ms = 0;  // milliseconds since the Unix epoch
    bool running = true;

    friend bool operator==(const Writer&, const Writer&) = default;
};

// The context as the JSON written to the file. `seq` increases with every write by one writer (a
// new viewer starts again at 1; its writer section says it is a new one); the timestamp is
// milliseconds since the Unix epoch. nlohmann::ordered_json keeps the fields in the order written,
// which makes the file easier to read than alphabetical order would.
[[nodiscard]] nlohmann::ordered_json to_json(const Context& context, const Writer& writer, std::uint64_t seq,
                                             std::int64_t written_at_ms);

// The context bar's lines, as text. Separate from drawing, so tests can check what the bar says.
struct ContextBarText {
    std::string background;            // always shown
    std::optional<std::string> point;  // shown when something is pointed at
    std::optional<std::string> follow; // shown when something is followed

    friend bool operator==(const ContextBarText&, const ContextBarText&) = default;
};

[[nodiscard]] ContextBarText bar_text(const Context& context);

}  // namespace fillscope
