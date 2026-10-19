#include "viewer/viewer_state.h"

#include <algorithm>
#include <format>

namespace fillscope {
namespace {

PriceEnvelope::Bucket merge(PriceEnvelope::Bucket a, PriceEnvelope::Bucket b) {
    return {.low = std::min(a.low, b.low), .high = std::max(a.high, b.high)};
}

}  // namespace

void PriceEnvelope::append(std::int64_t price) {
    if (buckets_.empty() || frames_in_last_bucket_ == frames_per_bucket_) {
        buckets_.push_back({.low = price, .high = price});
        frames_in_last_bucket_ = 1;
    } else {
        buckets_.back() = merge(buckets_.back(), {.low = price, .high = price});
        ++frames_in_last_bucket_;
    }
    if (buckets_.size() <= kMaxBuckets) {
        return;
    }
    // Too many: kMaxBuckets + 1, an odd count, since buckets are added one at a time and kMaxBuckets
    // is even. Every bucket but the last is full. Merge the full ones in pairs, halving their count
    // and doubling each one's span; the last, partly filled bucket stands alone and keeps its frames.
    const std::size_t pairs = buckets_.size() / 2;
    for (std::size_t i = 0; i < pairs; ++i) {
        buckets_[i] = merge(buckets_[2 * i], buckets_[2 * i + 1]);
    }
    buckets_[pairs] = buckets_.back();
    buckets_.resize(pairs + 1);
    frames_per_bucket_ *= 2;
}

void PriceEnvelope::reduce_into(std::size_t max_buckets, std::vector<Bucket>& out) const {
    out.clear();
    if (max_buckets == 0 || buckets_.size() <= max_buckets) {
        out.assign(buckets_.begin(), buckets_.end());
        return;
    }
    // Groups of `group` consecutive buckets, rounded up, so there are at most max_buckets of them.
    // The last group takes what is left, which may be fewer.
    const std::size_t group = (buckets_.size() + max_buckets - 1) / max_buckets;
    for (std::size_t start = 0; start < buckets_.size(); start += group) {
        const std::size_t end = std::min(start + group, buckets_.size());
        Bucket combined = buckets_[start];
        for (std::size_t i = start + 1; i < end; ++i) {
            combined = merge(combined, buckets_[i]);
        }
        out.push_back(combined);
    }
}

std::vector<PriceEnvelope::Bucket> PriceEnvelope::reduced(std::size_t max_buckets) const {
    std::vector<Bucket> result;
    reduce_into(max_buckets, result);
    return result;
}

void Run::append(const Frame& frame) {
    if (!index_break_ && frame.index != frames_.size()) {
        index_break_ = std::format("the run's frame indices didn't count 0, 1, 2, ...: frame {} of the run "
                                        "arrived numbered #{}",
                                        frames_.size() + 1, frame.index);
    }
    frames_.push_back(frame);
    indices_by_id_[frame.id].push_back(frame.index);  // operator[] creates the id's list the first time
    prices_.append(frame.price);
}

std::optional<std::string> Run::no_replay_reason() const {
    if (!hello_) {
        return "the source didn't describe its run, so there is no seed";
    }
    return index_break_;
}

std::span<const std::uint64_t> Run::indices_of(std::uint32_t id) const {
    const auto found = indices_by_id_.find(id);
    return found == indices_by_id_.end() ? std::span<const std::uint64_t>{} : std::span{found->second};
}

bool ViewerState::apply(const Message& message) {
    // std::visit returns what the chosen lambda returns, so both must return the same type (bool).
    return std::visit(Overloaded{
                          [&](const SourceHello& hello) {
                              run.emplace(++runs_started_, hello);  // destroys the old Run, builds the new one in place
                              return true;
                          },
                          [&](const Frame& frame) {
                              const bool started = !run.has_value();
                              if (started) {
                                  run.emplace(++runs_started_, std::nullopt);  // no description: can't be replayed
                              }
                              run->append(frame);
                              return started;
                          },
                      },
                      message);
}

}  // namespace fillscope
