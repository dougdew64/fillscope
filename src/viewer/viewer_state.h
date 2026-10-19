// What the viewer has received: the current run's frames, and the source's connection.
#pragma once

#include "channel/message.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fillscope {

// The frames of one run of a source, and the description that makes them reproducible.
//
// A class rather than a plain struct because it keeps an invariant: prices_ always has one entry
// per frame, in the same order. Only append() changes either vector, so the two can't drift apart.
class Run {
public:
    // A run starts with the source's description, or without one (std::nullopt) if frames arrived
    // from a source that didn't describe itself; then no frame of it has a replay address.
    explicit Run(std::optional<SourceHello> hello) : hello_{hello} {}

    void append(const Frame& frame);

    [[nodiscard]] const std::optional<SourceHello>& hello() const noexcept { return hello_; }

    // std::span is a non-owning view of contiguous elements: callers see the frames without
    // copying them and without being able to change them.
    [[nodiscard]] std::span<const Frame> frames() const noexcept { return frames_; }

    // The frames' prices as floats, which is what ImGui::PlotLines draws.
    [[nodiscard]] std::span<const float> prices() const noexcept { return prices_; }

private:
    std::optional<SourceHello> hello_;
    std::vector<Frame> frames_;
    std::vector<float> prices_;
};

struct ViewerState {
    // Empty until the first message arrives; replaced whenever a source describes a new run, so the
    // frames shown always belong to one run (indices restart at 0 in every run).
    std::optional<Run> run;
    bool source_connected = false;
    double seconds_since_frame = 0;
    std::uint64_t paints = 0;

    // Applies one message from the source. Returns true if it started a new run.
    bool apply(const Message& message);
};

}  // namespace fillscope
