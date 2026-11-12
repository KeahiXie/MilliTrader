#pragma once
// LatencyHistogram: log-linear buckets (HdrHistogram-style), O(1) record, fixed ~10 KB.
// One bucket per ns below 64 ns, then 32 sub-buckets per power of two, so percentiles are within ~3%.

#include <array>
#include <cstdint>

namespace mt {

class LatencyHistogram {
public:
    static constexpr int kSubBucketBits = 5;                 // 32 sub-buckets per power of two
    static constexpr int kSubBuckets = 1 << kSubBucketBits;
    static constexpr int kBucketCount = 64 + 40 * kSubBuckets;

    void record(int64_t value_ns) {
        if (value_ns < 0) value_ns = 0;
        counts_[index_of(static_cast<uint64_t>(value_ns))] += 1;
        ++total_;
        if (value_ns > max_) max_ = value_ns;
    }

    // Value at percentile p (0..100), reported as its bucket's upper edge so it never under-reports.
    int64_t percentile(double p) const {
        if (total_ == 0) return 0;
        const int64_t rank = static_cast<int64_t>(p / 100.0 * static_cast<double>(total_ - 1)) + 1;
        int64_t seen = 0;
        for (int i = 0; i < kBucketCount; ++i) {
            seen += counts_[i];
            if (seen >= rank) return upper_edge(i) < max_ ? upper_edge(i) : max_;
        }
        return max_;
    }

    int64_t count() const { return total_; }
    int64_t max() const { return max_; }
    void reset() { *this = LatencyHistogram{}; }

private:
    static int index_of(uint64_t value) {
        if (value < 64) return static_cast<int>(value);
        const int msb = 63 - __builtin_clzll(value);                 // value in [2^msb, 2^(msb+1))
        const int sub = static_cast<int>((value >> (msb - kSubBucketBits)) & (kSubBuckets - 1));
        const int index = 64 + (msb - 6) * kSubBuckets + sub;
        return index < kBucketCount ? index : kBucketCount - 1;
    }

    static int64_t upper_edge(int index) {
        if (index < 64) return index;
        const int msb = (index - 64) / kSubBuckets + 6;
        const int sub = (index - 64) % kSubBuckets;
        const int64_t base = int64_t{1} << msb;
        return base + (static_cast<int64_t>(sub) + 1) * (base >> kSubBucketBits) - 1;
    }

    std::array<int64_t, kBucketCount> counts_{};
    int64_t total_ = 0;
    int64_t max_ = 0;
};

}  // namespace mt
