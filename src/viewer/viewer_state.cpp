#include "viewer/viewer_state.h"

#include <algorithm>

namespace fillscope {
namespace {

PriceEnvelope::Bucket merge(PriceEnvelope::Bucket a, PriceEnvelope::Bucket b) {
    return {.low = std::min(a.low, b.low), .high = std::max(a.high, b.high)};
}

}  // namespace

void PriceEnvelope::append(float price) {
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
    // Too many: merge neighbours in pairs, halving the count and doubling each bucket's span. Every
    // bucket but the last is full; if the count is odd, the last stands alone and keeps its frames.
    const bool odd = buckets_.size() % 2 == 1;
    const std::size_t pairs = buckets_.size() / 2;
    for (std::size_t i = 0; i < pairs; ++i) {
        buckets_[i] = merge(buckets_[2 * i], buckets_[2 * i + 1]);
    }
    if (odd) {
        buckets_[pairs] = buckets_.back();
    } else {
        frames_in_last_bucket_ += frames_per_bucket_;  // the last pair held a full bucket and the last
    }
    buckets_.resize(pairs + (odd ? 1 : 0));
    frames_per_bucket_ *= 2;
}

std::vector<PriceEnvelope::Bucket> PriceEnvelope::reduced(std::size_t max_buckets) const {
    if (max_buckets == 0 || buckets_.size() <= max_buckets) {
        return buckets_;
    }
    // Groups of `group` consecutive buckets, rounded up, so there are at most max_buckets of them.
    const std::size_t group = (buckets_.size() + max_buckets - 1) / max_buckets;
    std::vector<Bucket> result;
    result.reserve(max_buckets);
    for (std::size_t start = 0; start < buckets_.size(); start += group) {
        const std::size_t end = std::min(start + group, buckets_.size());
        Bucket combined = buckets_[start];
        for (std::size_t i = start + 1; i < end; ++i) {
            combined = merge(combined, buckets_[i]);
        }
        result.push_back(combined);
    }
    return result;
}

void Run::append(const Frame& frame) {
    frames_.push_back(frame);
    indices_by_id_[frame.id].push_back(frame.index);  // operator[] creates the id's list the first time
    prices_.append(static_cast<float>(frame.price));
}

std::span<const std::uint64_t> Run::indices_of(std::uint32_t id) const {
    const auto found = indices_by_id_.find(id);
    return found == indices_by_id_.end() ? std::span<const std::uint64_t>{} : std::span{found->second};
}

bool ViewerState::apply(const Message& message) {
    // std::visit returns what the chosen lambda returns, so both must return the same type (bool).
    return std::visit(Overloaded{
                          [&](const SourceHello& hello) {
                              run.emplace(hello);  // emplace destroys the old Run and builds the new one in place
                              return true;
                          },
                          [&](const Frame& frame) {
                              const bool started = !run.has_value();
                              if (started) {
                                  run.emplace(std::nullopt);  // frames with no description: a run that can't be replayed
                              }
                              run->append(frame);
                              return started;
                          },
                      },
                      message);
}

}  // namespace fillscope
