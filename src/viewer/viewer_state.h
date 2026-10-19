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
// its bucket's low and high, so no spike is lost. Prices are kept exactly, as integers: a float
// would round any price above 2^24 (about 16.7 million) and could merge a spike into its
// neighbours.
class PriceEnvelope {
public:
    struct Bucket {
        std::int64_t low;
        std::int64_t high;
        friend bool operator==(const Bucket&, const Bucket&) = default;
    };

    // Even, so that a merge always happens at kMaxBuckets + 1 buckets, an odd count: the last bucket
    // then stands alone, and every other bucket stays full (see append()).
    static constexpr std::size_t kMaxBuckets = 4096;
    static_assert(kMaxBuckets % 2 == 0);

    void append(std::int64_t price);

    [[nodiscard]] std::span<const Bucket> buckets() const noexcept { return buckets_; }
    [[nodiscard]] std::size_t frames_per_bucket() const noexcept { return frames_per_bucket_; }

    // How many prices the last bucket holds; every other bucket holds frames_per_bucket().
    [[nodiscard]] std::size_t frames_in_last_bucket() const noexcept { return frames_in_last_bucket_; }

    // The envelope merged down to at most `max_buckets` buckets (all of them if `max_buckets` is 0),
    // for a plot that many pixels wide, written into `out` so a caller drawing every paint can reuse
    // one vector's storage. Every price is inside one of them; the last may hold fewer buckets than
    // the others. Costs O(buckets), at most kMaxBuckets, however long the run.
    void reduce_into(std::size_t max_buckets, std::vector<Bucket>& out) const;
    [[nodiscard]] std::vector<Bucket> reduced(std::size_t max_buckets) const;

private:
    std::vector<Bucket> buckets_;
    std::size_t frames_per_bucket_ = 1;
    std::size_t frames_in_last_bucket_ = 0;
};

// The frames of one run of a source, and the description that makes them reproducible.
//
// A class rather than a plain struct because it keeps invariants: the frames, the index from id to
// frames, the price envelope and the check of the frames' indices always describe the same frames.
// Only append() changes any of them, so they can't fall out of step.
//
// Every frame is kept, about 32 bytes each with its entry in the index by id: 10 million frames
// take about 320 MB. The stand-in source sends 1,000 by default; a run that long is not expected
// before real frames replace the stand-in, and would need a bound then.
class Run {
public:
    // A run starts with the source's description, or without one (std::nullopt) if frames arrived
    // from a source that didn't describe itself; then no frame of it has a replay address.
    // `number` tells this run from the viewer's other runs (1 for the first; see ViewerState).
    Run(std::uint64_t number, std::optional<SourceHello> hello) : number_{number}, hello_{hello} {}

    void append(const Frame& frame);

    [[nodiscard]] std::uint64_t number() const noexcept { return number_; }
    [[nodiscard]] const std::optional<SourceHello>& hello() const noexcept { return hello_; }

    // Why this run's frames have no replay address, or nothing if they have one. A replay address
    // is (seed, index), which names the frame the frame source produces at that index from that
    // seed. That is the frame received only if the run has a seed and its frames arrived with
    // indices 0, 1, 2, ... in order; the first frame that breaks the sequence ends replay addresses
    // for the rest of the run.
    [[nodiscard]] std::optional<std::string> no_replay_reason() const;

    // std::span is a non-owning view of contiguous elements: callers see the frames without
    // copying them and without being able to change them.
    [[nodiscard]] std::span<const Frame> frames() const noexcept { return frames_; }

    // The indices of the frames with this id, in order; empty if none has it. Kept up to date by
    // append(), so following an id costs nothing per paint, however long the run.
    [[nodiscard]] std::span<const std::uint64_t> indices_of(std::uint32_t id) const;

    [[nodiscard]] const PriceEnvelope& prices() const noexcept { return prices_; }

private:
    std::uint64_t number_;
    std::optional<SourceHello> hello_;
    std::optional<std::string> index_break_;  // the first break in the sequence of indices
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
    // Why the last source was dropped, or couldn't be accepted, if that happened.
    std::optional<std::string> source_error;

    // A new source has sent its first message: what came before belonged to another.
    void begin_connection() { run.reset(); }

    // Applies one message from the source. Returns true if it started a new run.
    bool apply(const Message& message);

private:
    // Each run gets the next number, so two runs are never confused, even though both number their
    // frames from 0.
    std::uint64_t runs_started_ = 0;
};

}  // namespace fillscope
