// Latency histogram with exact nanosecond buckets, preallocated up front.
//
// Recording a sample is one array increment: no allocation, no sorting, so
// measuring doesn't disturb what's being measured. Percentiles are exact for
// samples up to `max_ns`; anything slower lands in an overflow bucket, and the
// true maximum is always tracked separately.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ob {

class LatencyRecorder {
public:
    explicit LatencyRecorder(std::uint64_t max_ns = 1'000'000) : max_ns_(max_ns), buckets_(max_ns + 1, 0) {}

    void record(std::uint64_t ns) {
        if (ns <= max_ns_) ++buckets_[ns];
        else               ++overflow_;
        ++count_;
        sum_ += ns;
        max_ = std::max(max_, ns);
        if (ns != 0) min_nonzero_ = std::min(min_nonzero_, ns);
    }

    // Smallest value v such that at least p% of samples are <= v.
    // If that falls in the overflow bucket, returns the max (an upper bound).
    std::uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        const auto rank = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(count_)));
        std::uint64_t seen = 0;
        for (std::uint64_t ns = 0; ns <= max_ns_; ++ns) {
            seen += buckets_[ns];
            if (seen >= rank && seen != 0) return ns;
        }
        return max_;
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t max() const { return max_; }
    std::uint64_t overflow() const { return overflow_; }
    std::uint64_t min_nonzero() const { return min_nonzero_ == UINT64_MAX ? 0 : min_nonzero_; }
    double        mean() const { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

private:
    std::uint64_t              max_ns_;
    std::vector<std::uint64_t> buckets_;
    std::uint64_t              overflow_    = 0;
    std::uint64_t              count_       = 0;
    std::uint64_t              sum_         = 0;
    std::uint64_t              max_         = 0;
    std::uint64_t              min_nonzero_ = UINT64_MAX;
};

}  // namespace ob
