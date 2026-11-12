#pragma once
// StatsCollector: hot-path threads push HopSamples into their own SPSC queues (no locks, no shared
// counters); the stats thread aggregates them into histograms and forwards StatusSnapshots to the
// dashboard socket without blocking (dropped if nobody is listening).

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/spsc_queue.hpp"
#include "core/status.hpp"
#include "io/latency_histogram.hpp"

namespace mt {

struct HopSample {
    uint8_t hop = 0;      // Hop enum from status.hpp
    int64_t nanos = 0;
};

using HopQueue = SpscQueue<HopSample>;
using StatusQueue = SpscQueue<StatusSnapshot>;

class StatsCollector {
public:
    StatsCollector();
    ~StatsCollector();

    // One queue per producer thread (SPSC).
    HopQueue& add_producer(size_t capacity = 1 << 16);
    StatusQueue& status_queue() { return status_queue_; }

    void start(bool publish_to_dashboard);
    void stop();

    // Read after stop(); reading while running is racy.
    const std::array<LatencyHistogram, kHopCount>& histograms() const { return histograms_; }
    std::string summary_table() const;  // Markdown table of every hop

private:
    void run();
    void publish(StatusSnapshot& status);

    std::vector<std::unique_ptr<HopQueue>> producers_;
    StatusQueue status_queue_;
    std::array<LatencyHistogram, kHopCount> histograms_{};
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool publish_ = false;
    int socket_fd_ = -1;
};

}  // namespace mt
