#include "viewer/context.h"

#include <format>
#include <ranges>

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
    if (pointed_) {
        clear_point();
    }
}

Context make_context(const ViewerState& state, const Selection& selection) {
    Context context;
    context.source.connected = state.source_connected;

    // A run's seed, if it has one: what turns a frame index into a replay address.
    std::optional<std::uint64_t> seed;
    if (state.run) {
        const Run& run = *state.run;
        context.source.run = run.hello();
        context.source.frames_received = run.frames().size();
        if (!run.frames().empty()) {
            context.source.latest_frame_index = run.frames().back().index;
        }
        if (run.hello()) {
            seed = run.hello()->seed;
        }
    }

    if (const auto& frame = selection.pointed()) {
        context.point = Context::Point{.seq = selection.point_seq(), .frame = *frame, .replay = std::nullopt};
        if (seed) {
            context.point->replay = ReplayAddress{.seed = *seed, .frame_index = frame->index};
        }
    }

    if (const auto id = selection.followed()) {
        Context::Follow follow{.seq = selection.follow_seq(), .id = *id, .frame_indices = {}, .frame_count = 0, .seed = seed};
        if (state.run) {
            // A ranges pipeline: keep the frames with this id, then take each one's index. Views
            // are lazy, so nothing is copied until the loop pulls each element through.
            auto indices = state.run->frames()
                           | std::views::filter([&](const Frame& f) { return f.id == *id; })
                           | std::views::transform(&Frame::index);
            for (const std::uint64_t index : indices) {
                if (follow.frame_indices.size() < kMaxFollowedIndices) {
                    follow.frame_indices.push_back(index);
                }
                ++follow.frame_count;
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

nlohmann::ordered_json frame_json(const Frame& frame) {
    return {{"index", frame.index}, {"id", frame.id}, {"price", frame.price}, {"qty", frame.qty}};
}

}  // namespace

nlohmann::ordered_json to_json(const Context& context, std::uint64_t seq, std::int64_t written_at_ms) {
    nlohmann::ordered_json json;
    json["version"] = 1;
    json["seq"] = seq;
    json["writtenAtMs"] = written_at_ms;
    json["about"] =
        "What the Fillscope viewer's user has pointed at and followed. 'point' is one frame, the subject; "
        "'follow' is one id across the frames where it appears. Each section's seq says when it last "
        "changed: the higher one is what the user did last. A replay address (seed, frame index) "
        "identifies one exact state, reproducible by running the frame source with that seed.";

    const Context::Source& source = context.source;
    nlohmann::ordered_json run = nullptr;
    if (source.run) {
        run = {{"seed", source.run->seed}, {"frameCount", source.run->frame_count}};
    }
    json["background"] = {
        {"source", {{"connected", source.connected}, {"run", run}}},
        {"framesReceived", source.frames_received},
        {"latestFrameIndex", or_null(source.latest_frame_index)},
    };

    if (context.point) {
        const Context::Point& point = *context.point;
        nlohmann::ordered_json replay = nullptr;
        if (point.replay) {
            replay = {{"seed", point.replay->seed}, {"frameIndex", point.replay->frame_index}};
        }
        json["point"] = {{"seq", point.seq}, {"frame", frame_json(point.frame)}, {"replayAddress", replay}};
        if (!point.replay) {
            json["point"]["replayAddressMissing"] = "the source didn't describe its run, so there is no seed";
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
    const char* connection = source.connected ? "connected" : "not connected";
    if (source.run) {
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
