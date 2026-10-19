#include "viewer/context.h"

#include <algorithm>
#include <format>
#include <span>

namespace fillscope {

void Selection::point_at(const Frame& frame) {
    pointed_ = frame;
    point_seq_ = next_seq_++;
}

void Selection::clear_point() {
    pointed_.reset();
    point_seq_ = next_seq_++;
}

void Selection::follow(std::uint32_t id) {
    followed_ = id;
    follow_seq_ = next_seq_++;
}

void Selection::clear_follow() {
    followed_.reset();
    follow_seq_ = next_seq_++;
}

void Selection::new_run() {
    pointed_.reset();  // point_seq_ unchanged: the user didn't do this
}

Context make_context(const ViewerState& state, const Selection& selection) {
    Context context;
    context.source.connected = state.source_connected;
    context.source.error = state.source_error;

    // A run's seed, if it has one: what turns a frame index into a replay address.
    std::optional<std::uint64_t> seed;
    if (state.run) {
        const Run& run = *state.run;
        context.source.run_number = run.number();
        context.source.run = run.hello();
        context.source.no_replay_reason = run.no_replay_reason();
        context.source.frames_received = run.frames().size();
        if (!run.frames().empty()) {
            context.source.latest_frame_index = run.frames().back().index;
        }
        if (!context.source.no_replay_reason) {
            seed = run.hello()->seed;  // no reason means the run has a description, so a seed
        }
    }

    if (const auto& frame = selection.pointed()) {
        context.point = Context::Point{.seq = selection.point_seq(), .frame = *frame, .replay = std::nullopt};
        if (seed) {
            context.point->replay = ReplayAddress{.seed = *seed, .frame_index = frame->index};
        }
    }

    if (const auto id = selection.followed()) {
        // The run keeps each id's frame indices as frames arrive, so this costs at most
        // kMaxFollowedIndices copies per paint, not a scan of the run.
        const std::span<const std::uint64_t> indices =
            state.run ? state.run->indices_of(*id) : std::span<const std::uint64_t>{};
        const auto listed = indices.first(std::min(indices.size(), kMaxFollowedIndices));
        context.follow = Context::Follow{
            .seq = selection.follow_seq(),
            .id = *id,
            .frame_indices = {listed.begin(), listed.end()},
            .frame_count = indices.size(),
            .seed = seed,
        };
    }
    return context;
}

namespace {

// A std::optional as JSON: its value, or null when it is empty.
template <typename T>
nlohmann::ordered_json or_null(const std::optional<T>& value) {
    return value ? nlohmann::ordered_json(*value) : nlohmann::ordered_json(nullptr);
}

nlohmann::ordered_json frame_json(const Frame& frame) {
    return {{"index", frame.index}, {"id", frame.id}, {"price", frame.price}, {"qty", frame.qty}};
}

}  // namespace

nlohmann::ordered_json to_json(const Context& context, const Writer& writer, std::uint64_t seq,
                               std::int64_t written_at_ms) {
    nlohmann::ordered_json json;
    json["version"] = 1;
    json["seq"] = seq;
    json["writtenAtMs"] = written_at_ms;
    json["writer"] = {{"program", "viewer"}, {"pid", writer.pid}, {"startedAtMs", writer.started_at_ms},
                      {"running", writer.running}};
    json["about"] =
        "What the Fillscope viewer's user has pointed at and followed. 'point' is one frame, the subject; "
        "'follow' is one id across the frames where it appears. Each section's seq is taken when the user "
        "changes that section, so the higher one is what the user did last (a section the viewer clears "
        "itself, as a new run clears the point, takes none). A replay address (seed, frame index) "
        "identifies one exact state, reproducible by running the frame source with that seed; when the run "
        "has none, background.noReplayAddress says why. 'writer' "
        "says which viewer wrote this file and whether it is still running.";

    const Context::Source& source = context.source;
    nlohmann::ordered_json run = nullptr;
    if (source.run) {
        run = {{"seed", source.run->seed}, {"frameCount", source.run->frame_count}};
    }
    json["background"] = {
        {"source", {{"connected", source.connected}, {"run", run}, {"error", or_null(source.error)}}},
        {"runNumber", or_null(source.run_number)},
        {"framesReceived", source.frames_received},
        {"moreFramesThanDescribed", source.more_frames_than_described()},
        {"latestFrameIndex", or_null(source.latest_frame_index)},
        {"noReplayAddress", or_null(source.no_replay_reason)},
    };

    if (context.point) {
        const Context::Point& point = *context.point;
        nlohmann::ordered_json replay = nullptr;
        if (point.replay) {
            replay = {{"seed", point.replay->seed}, {"frameIndex", point.replay->frame_index}};
        }
        json["point"] = {{"seq", point.seq}, {"frame", frame_json(point.frame)}, {"replayAddress", replay}};
        if (!point.replay) {
            json["point"]["replayAddressMissing"] = source.no_replay_reason.value_or("no run");
        }
    } else {
        json["point"] = nullptr;
    }

    if (context.follow) {
        const Context::Follow& follow = *context.follow;
        json["follow"] = {
            {"seq", follow.seq},
            {"id", follow.id},
            {"frameCount", follow.frame_count},
            {"frameIndices", follow.frame_indices},
            {"frameIndicesTruncated", follow.truncated()},
            {"seed", or_null(follow.seed)},
        };
    } else {
        json["follow"] = nullptr;
    }
    return json;
}

ContextBarText bar_text(const Context& context) {
    ContextBarText text;
    const Context::Source& source = context.source;
    const std::string connection = source.connected ? "connected"
                                   : source.error   ? std::format("not connected (the last source was dropped: {})", *source.error)
                                                    : "not connected";
    if (source.more_frames_than_described()) {
        text.background = std::format("source seed {} · {} frames (the source said {}) · {}", source.run->seed,
                                      source.frames_received, source.run->frame_count, connection);
    } else if (source.run) {
        text.background = std::format("source seed {} · {} of {} frames · {}", source.run->seed,
                                      source.frames_received, source.run->frame_count, connection);
    } else if (source.frames_received > 0) {
        text.background = std::format("a source that didn't describe its run · {} frames · {}",
                                      source.frames_received, connection);
    } else {
        text.background = std::format("no frames yet · {}", connection);
    }

    if (context.point) {
        const Frame& frame = context.point->frame;
        text.point = std::format("Pointing at frame #{} (id {}, price {}, qty {}){}", frame.index, frame.id,
                                 frame.price, frame.qty, context.point->replay ? "" : " · no replay address");
    }
    if (context.follow) {
        const Context::Follow& follow = *context.follow;
        text.follow = std::format("Following id {}: {} frame{}{}", follow.id, follow.frame_count,
                                  follow.frame_count == 1 ? "" : "s",
                                  follow.truncated() ? std::format(" (first {} listed)", follow.frame_indices.size())
                                                     : std::string{});
    }
    return text;
}

}  // namespace fillscope
