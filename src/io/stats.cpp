#include "io/stats.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/clock.hpp"
#include "core/thread_utils.hpp"

namespace mt {

StatsCollector::StatsCollector() : status_queue_(64) {}

StatsCollector::~StatsCollector() { stop(); }

HopQueue& StatsCollector::add_producer(size_t capacity) {
    producers_.push_back(std::make_unique<HopQueue>(capacity));
    return *producers_.back();
}

void StatsCollector::start(bool publish_to_dashboard) {
    publish_ = publish_to_dashboard;
    if (publish_) {
        socket_fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    }
    running_ = true;
    thread_ = start_thread("mt-stats", -1, [this] { run(); });
}

void StatsCollector::stop() {
    if (!running_.exchange(false)) return;
    thread_.join();
    if (socket_fd_ >= 0) close(socket_fd_);
    socket_fd_ = -1;
}

void StatsCollector::publish(StatusSnapshot& status) {
    for (int hop = 0; hop < kHopCount; ++hop) {
        status.hops[hop].count = histograms_[hop].count();
        status.hops[hop].p50_ns = histograms_[hop].percentile(50);
        status.hops[hop].p99_ns = histograms_[hop].percentile(99);
        status.hops[hop].p999_ns = histograms_[hop].percentile(99.9);
    }
    if (socket_fd_ < 0) return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, kStatusSocketPath, sizeof(address.sun_path) - 1);
    // Non-blocking: drop the snapshot if the dashboard is slow or absent.
    sendto(socket_fd_, &status, sizeof(status), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&address), sizeof(address));
}

void StatsCollector::run() {
    HopSample sample;
    StatusSnapshot status;
    while (running_) {
        bool any = false;
        for (auto& producer : producers_) {
            while (producer->try_pop(sample)) {
                if (sample.hop < kHopCount) histograms_[sample.hop].record(sample.nanos);
                any = true;
            }
        }
        while (status_queue_.try_pop(status)) {
            publish(status);
            any = true;
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (auto& producer : producers_) {
        while (producer->try_pop(sample)) {
            if (sample.hop < kHopCount) histograms_[sample.hop].record(sample.nanos);
        }
    }
}

std::string StatsCollector::summary_table() const {
    std::string table = "| hop | samples | p50 | p99 | p99.9 | max |\n|---|---:|---:|---:|---:|---:|\n";
    auto format = [](int64_t ns) {
        char buffer[32];
        if (ns < 10'000) std::snprintf(buffer, sizeof(buffer), "%lld ns", static_cast<long long>(ns));
        else if (ns < 10'000'000) std::snprintf(buffer, sizeof(buffer), "%.1f us", static_cast<double>(ns) / 1e3);
        else std::snprintf(buffer, sizeof(buffer), "%.1f ms", static_cast<double>(ns) / 1e6);
        return std::string(buffer);
    };
    for (int hop = 0; hop < kHopCount; ++hop) {
        const LatencyHistogram& h = histograms_[hop];
        table += std::string("| ") + kHopNames[hop] + " | " + std::to_string(h.count()) + " | " +
                 format(h.percentile(50)) + " | " + format(h.percentile(99)) + " | " + format(h.percentile(99.9)) +
                 " | " + format(h.max()) + " |\n";
    }
    return table;
}

}  // namespace mt
