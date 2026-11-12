#pragma once
// Clocks: wall clock stamps received frames (can jump), monotonic is for latency only.
// The engine never reads either for a decision; "now" comes from the event, so replay is exact.

#include <cstdint>
#include <ctime>
#include <string>

namespace mt {

inline int64_t wall_clock_ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

inline int64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

inline constexpr int64_t kNanosPerMilli = 1'000'000;
inline constexpr int64_t kNanosPerSecond = 1'000'000'000;

// Spin-wait hint (PAUSE on x86, YIELD on ARM): saves power and frees the hyper-thread sibling.
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// "2026-11-11T20:45:00.123Z" (UTC, millisecond precision).
std::string format_utc_iso8601(int64_t wall_ns);
// "2026-11-11" and "20" for building hourly file paths (UTC).
std::string utc_date(int64_t wall_ns);
std::string utc_hour(int64_t wall_ns);
// Parses "2026-11-12T01:45:00Z" or with fractional seconds; returns 0 on failure.
int64_t parse_utc_iso8601(const std::string& text);

}  // namespace mt
