#include "core/clock.hpp"

#include <cstdio>

namespace mt {

namespace {

tm to_utc_tm(int64_t wall_ns) {
    const time_t seconds = static_cast<time_t>(wall_ns / kNanosPerSecond);
    tm parts{};
    gmtime_r(&seconds, &parts);
    return parts;
}

}  // namespace

std::string format_utc_iso8601(int64_t wall_ns) {
    const tm parts = to_utc_tm(wall_ns);
    const int millis = static_cast<int>((wall_ns % kNanosPerSecond) / kNanosPerMilli);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec, millis);
    return buffer;
}

std::string utc_date(int64_t wall_ns) {
    const tm parts = to_utc_tm(wall_ns);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1,
                  parts.tm_mday);
    return buffer;
}

std::string utc_hour(int64_t wall_ns) {
    const tm parts = to_utc_tm(wall_ns);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d", parts.tm_hour);
    return buffer;
}

int64_t parse_utc_iso8601(const std::string& text) {
    tm parts{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
    const time_t seconds = timegm(&parts);

    // Optional fraction: ".123456Z"
    int64_t fraction_ns = 0;
    const size_t dot = text.find('.');
    if (dot != std::string::npos) {
        int64_t scale = 100'000'000;
        for (size_t i = dot + 1; i < text.size() && text[i] >= '0' && text[i] <= '9' && scale > 0; ++i) {
            fraction_ns += (text[i] - '0') * scale;
            scale /= 10;
        }
    }
    return static_cast<int64_t>(seconds) * kNanosPerSecond + fraction_ns;
}

}  // namespace mt
