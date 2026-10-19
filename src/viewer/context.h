// What the viewer's user has pointed at and followed, and the context published from it for the
// assistant working beside the viewer (docs/DECISIONS.md, 2026-10-19).
//
// The user assembles the subject of a question by pointing and clicking; the question itself is
// asked of the assistant, in another program. So the viewer can't wait to see the question: it
// publishes the assembled context as soon as it changes (context_publisher.h). Two ways to assemble
// it, kept apart because they mean different things:
//   - point at: one frame, the subject of a question;
//   - follow:   one id across every frame where it appears, a lens on the question.
// Each has its own sequence number, taken only when the user changes it, so the assistant can tell
// which the user did last.
#pragma once

#include "channel/message.h"
#include "viewer/viewer_state.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fillscope {

// The user's choices: what is pointed at and what is followed. Changed only through the member
// functions, which keep the sequence numbers right.
class Selection {
public:
    void point_at(const Frame& frame);
    void clear_point();
    void follow(std::uint32_t id);
    void clear_follow();

    // A new run has started: the frame pointed at belonged to the old one, so it is cleared. What is
    // followed is an id, which is just as meaningful in the new run, so it stays. The viewer did
    // this, not the user, so it takes no sequence number: the numbers say what the user did.
    void new_run();

    [[nodiscard]] const std::optional<Frame>& pointed() const noexcept { return pointed_; }
    [[nodiscard]] std::optional<std::uint32_t> followed() const noexcept { return followed_; }
    // When each last changed, on one shared counter, so the two can be compared.
    [[nodiscard]] std::uint64_t point_seq() const noexcept { return point_seq_; }
    [[nodiscard]] std::uint64_t follow_seq() const noexcept { return follow_seq_; }

private:
    std::optional<Frame> pointed_;
    std::optional<std::uint32_t> followed_;
    std::uint64_t point_seq_ = 0;
    std::uint64_t follow_seq_ = 0;
    std::uint64_t next_seq_ = 1;
};

// A frame's replay address: the run's seed and the frame's index identify one exact state, which
// the frame source can reproduce by running with that seed.
struct ReplayAddress {
    std::uint64_t seed;
    std::uint64_t frame_index;

    friend bool operator==(const ReplayAddress&, const ReplayAddress&) = default;
};

// At most this many frame indices are listed for a followed id; the count is always exact.
inline constexpr std::size_t kMaxFollowedIndices = 1000;

// Everything published at one moment: a value, built from the viewer's state and the selection by
// make_context. The context bar draws the Context that was last published, and the file holds the
// same Context as JSON, so the bar can't show anything the file doesn't say.
struct Context {
    struct Source {
        bool connected = false;
        std::optional<SourceHello> run;      // the run's description; empty if none was given
        std::size_t frames_received = 0;     // in the current run
        std::optional<std::uint64_t> latest_frame_index;
        std::optional<std::string> error;    // why the last source was dropped, if it was

        friend bool operator==(const Source&, const Source&) = default;
    };
    struct Point {
        std::uint64_t seq;
        Frame frame;
        std::optional<ReplayAddress> replay;  // empty when the run has no description

        friend bool operator==(const Point&, const Point&) = default;
    };
    struct Follow {
        std::uint64_t seq;
        std::uint32_t id;
        std::vector<std::uint64_t> frame_indices;  // where the id appears, up to kMaxFollowedIndices
        std::size_t frame_count = 0;               // where it appears, all of them
        std::optional<std::uint64_t> seed;         // with an index, a replay address

        [[nodiscard]] bool truncated() const noexcept { return frame_count > frame_indices.size(); }
        friend bool operator==(const Follow&, const Follow&) = default;
    };

    Source source;
    std::optional<Point> point;
    std::optional<Follow> follow;

    friend bool operator==(const Context&, const Context&) = default;
};

[[nodiscard]] Context make_context(const ViewerState& state, const Selection& selection);

// Who wrote the file. The file outlives the viewer, so a reader needs to tell a current file from
// one a viewer left behind: `running` is false in the last file a viewer writes as it exits, and
// `pid` names the process (if it is still running, /proc/<pid> exists). A viewer that crashes
// can't write its last file, so `running: true` with a pid that no longer exists means it crashed.
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
