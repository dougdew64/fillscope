// What the viewer has received: the current connection's run of frames, and the source's state.
#pragma once

#include "channel/message.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fillscope {

// A run's prices as a min/max envelope, for plotting a run of any length.
//
// The problem: a plot is a few hundred pixels wide, and a run can have millions of frames. Drawing
// every price costs time in proportion to the run on every paint; drawing every hundredth price
// hides the spikes between them, and a hidden spike is a plot that isn't true.
//
// So prices go into buckets, each holding the lowest and highest price of a stretch of consecutive
// frames. When there are more than kMaxBuckets, neighbouring buckets merge in pairs and each now
// covers twice as many frames. Appending costs O(1) (amortized: a merge touches every bucket, but
// happens only each time the run doubles), memory stays bounded, and every price is still inside
// its bucket's low and high, so no spike is lost.
class PriceEnvelope {
public:
    struct Bucket {
        float low;
        float high;
        friend bool operator==(const Bucket&, const Bucket&) = default;
    };

    static constexpr std::size_t kMaxBuckets = 4096;

    void append(float price);

    [[nodiscard]] std::span<const Bucket> buckets() const noexcept { return buckets_; }
    [[nodiscard]] std::size_t frames_per_bucket() const noexcept { return frames_per_bucket_; }

    // The envelope merged down to at most `max_buckets` buckets, for a plot that many buckets wide.
    // Costs O(buckets), at most kMaxBuckets, however long the run.
    [[nodiscard]] std::vector<Bucket> reduced(std::size_t max_buckets) const;

private:
    std::vector<Bucket> buckets_;
    std::size_t frames_per_bucket_ = 1;
    std::size_t frames_in_last_bucket_ = 0;
};

// The frames of one run of a source, and the description that makes them reproducible.
//
// A class rather than a plain struct because it keeps invariants: the frames, the index from id to
// frames, and the price envelope always describe the same frames. Only append() changes any of
// them, so they can't fall out of step.
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

    // The indices of the frames with this id, in order; empty if none has it. Kept up to date by
    // append(), so following an id costs nothing per paint, however long the run.
    [[nodiscard]] std::span<const std::uint64_t> indices_of(std::uint32_t id) const;

    // The prices, as floats: exact up to 2^24 (about 16.7 million), which the stand-in prices,
    // around 10,000, are far below.
    [[nodiscard]] const PriceEnvelope& prices() const noexcept { return prices_; }

private:
    std::optional<SourceHello> hello_;
    std::vector<Frame> frames_;
    std::unordered_map<std::uint32_t, std::vector<std::uint64_t>> indices_by_id_;
    PriceEnvelope prices_;
};

// What the viewer knows of its source.
struct ViewerState {
    // Empty until the current connection's first message arrives. A new connection clears it, and a
    // SourceHello replaces it, so the frames shown always come from one run of one source (frame
    // indices restart at 0 in every run, so frames of two runs must never mix).
    std::optional<Run> run;
    bool source_connected = false;
    // Why the last source was dropped for sending something that wasn't a message, if it was.
    std::optional<std::string> source_error;

    // A new source has connected: what came before belonged to another.
    void begin_connection() { run.reset(); }

    // Applies one message from the source. Returns true if it started a new run.
    bool apply(const Message& message);
};

}  // namespace fillscope
