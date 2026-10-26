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

// Why a live viewer's context has no replay address: only a recorded run can be rebuilt exactly.
inline constexpr std::string_view kNoReplayAddress =
    "live: the viewer follows the exchange as it runs, which no recording rebuilds; while the viewer runs, a "
    "keptAddress rebuilds what it shows. A recorded run can always be rebuilt: exchange_main --record FILE, then "
    "exchange_replay FILE MARKET_DATA, then viewer --replay MARKET_DATA";

// One exact state of a recorded run: the books after update `seq` of a replay's market data, which
// exchange_replay made from the recording. Rebuilt by applying the file's updates 1 to `seq`.
struct ReplayAddress {
    std::string market_data_file;
    std::string recording;
    std::uint64_t seq = 0;

    friend bool operator==(const ReplayAddress&, const ReplayAddress&) = default;
};

// Where the user is in an answer (viewer --answer): which answer, and which step of it. A question
// asked now is about this step: its books are the replay's after it, or, while it asks, before it.
struct AnswerPlace {
    std::string file;       // the answer file
    std::string question;   // the question it answers
    std::size_t step = 0;   // 0 for its intro, else its step, from 1
    std::size_t steps = 0;  // how many it has
    bool asking = false;    // the step's question is shown, and the step not yet played
    std::string lesson;     // the course's lesson it is ("lesson 2, \"The spread\""); empty for a question

    friend bool operator==(const AnswerPlace&, const AnswerPlace&) = default;
};

// An order as the books have it now: resting, with its place in its price's queue; or not resting,
// with the last update applied to it (how it left, if a cancel) or the recovery it left during; or
// never seen.
struct OrderState {
    OrderRef ref;
    std::optional<Order> resting;
    std::size_t orders_ahead = 0;     // when resting: the orders before it in its price's queue
    std::uint64_t qty_ahead = 0;      // and their total quantity
    std::optional<OrderEvent> last_event;  // empty if the books never applied an update to it
    // When it isn't resting and its last update seen isn't a cancel: the recovery it left during,
    // whose updates were never seen one by one, so how it left isn't known.
    std::optional<Recovery> left_during;

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
        std::uint64_t last_seq = 0;  // the feed's books are as of this incremental update
        // The books shown, and what is pointed at and followed in them, are as of this one: last_seq,
        // unless time-shifted.
        std::uint64_t books_seq = 0;
        std::uint64_t updates_received = 0;
        std::optional<std::string> recovering_because;
        std::size_t recovery_count = 0;
        std::vector<Recovery> latest_recoveries;  // up to kMaxListedRecoveries, oldest first
        std::optional<std::string> error;        // the last datagram that couldn't be received or decoded
        std::uint32_t shown_ticker = 0;
        // A replay's market data file and recording, and how many updates it has; empty when live.
        std::optional<ReplayAddress> replay;  // seq: the replay's last update
        // In an answer, where the user is in it; empty otherwise, and while none is shown.
        std::optional<AnswerPlace> answer;
        // A viewer showing answers, with none to show: why (none written yet, or why the last can't be
        // shown). Empty otherwise.
        std::optional<std::string> no_answer;
        // Live, while the screen shows a moment behind the newest (time-shifted): which, and how far
        // behind; empty while it follows live. The status and last_seq above are the feed's, live.
        struct TimeShifted {
            std::uint64_t shown_seq = 0;
            std::uint64_t newest_seq = 0;
            std::optional<double> behind_seconds;  // between the two updates' arrivals at the viewer
            bool playing = false;
            friend bool operator==(const TimeShifted&, const TimeShifted&) = default;
        };
        std::optional<TimeShifted> time_shift;
        // Live, while the viewer runs: the file it writes what it keeps to (feed/kept_file.h), and
        // whether the books shown, as of books_seq, are a moment in it: then a point and a follow
        // have an address there, from which the books are rebuilt (fillscope-books).
        std::optional<std::string> kept_file;
        bool books_kept = false;

        friend bool operator==(const Feed&, const Feed&) = default;
    };
    struct Point {
        std::uint64_t seq = 0;
        std::variant<OrderState, LevelState> target;
        std::optional<ReplayAddress> replay;  // the state it describes, when it is a replay's

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
        std::optional<ReplayAddress> replay;  // the state it describes, when it is a replay's

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
