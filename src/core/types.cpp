#include "core/types.hpp"

#include <algorithm>

#include <cstdio>
#include <cstring>

namespace mt {

namespace {

// Parses decimal text into an integer scaled by 10^decimals: ("0.56", 4) -> 5600.
// Digits past `decimals` must be zeros, unless round_down drops them.
std::optional<int64_t> parse_scaled(std::string_view text, int decimals, bool allow_negative,
                                    bool round_down) {
    if (text.empty()) return std::nullopt;

    bool negative = false;
    size_t i = 0;
    if (text[0] == '-') {
        if (!allow_negative) return std::nullopt;
        negative = true;
        i = 1;
    }

    int64_t whole = 0;
    int whole_digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        if (whole_digits >= 15) return std::nullopt;  // would overflow int64 after scaling
        whole = whole * 10 + (text[i] - '0');
        ++whole_digits;
        ++i;
    }

    int64_t fraction = 0;
    int fraction_digits = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            const int digit = text[i] - '0';
            if (fraction_digits < decimals) {
                fraction = fraction * 10 + digit;
                ++fraction_digits;
            } else if (digit != 0 && !round_down) {
                return std::nullopt;  // more precision than the unit can hold
            }
            ++i;
        }
        if (whole_digits == 0 && fraction_digits == 0) return std::nullopt;  // just "."
    } else if (whole_digits == 0) {
        return std::nullopt;
    }

    if (i != text.size()) return std::nullopt;  // trailing junk like "0.5x"

    // pad to the full number of decimals: "0.56" -> 5600
    while (fraction_digits < decimals) {
        fraction *= 10;
        ++fraction_digits;
    }

    int64_t scale = 1;
    for (int d = 0; d < decimals; ++d) scale *= 10;

    const int64_t value = whole * scale + fraction;
    return negative ? -value : value;
}

std::string format_scaled(int64_t value, int decimals) {
    const bool negative = value < 0;
    const uint64_t magnitude = negative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);

    uint64_t scale = 1;
    for (int d = 0; d < decimals; ++d) scale *= 10;

    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%s%llu.%0*llu", negative ? "-" : "",
                  static_cast<unsigned long long>(magnitude / scale), decimals,
                  static_cast<unsigned long long>(magnitude % scale));
    return buffer;
}

}  // namespace

std::optional<Price> parse_price(std::string_view dollars) {
    const auto ticks = parse_scaled(dollars, 4, /*allow_negative=*/false, /*round_down=*/false);
    if (!ticks || *ticks > kTicksPerDollar) return std::nullopt;
    return Price{static_cast<int32_t>(*ticks)};
}

std::optional<Qty> parse_qty(std::string_view contracts) {
    const auto hundredths = parse_scaled(contracts, 2, /*allow_negative=*/true, /*round_down=*/false);
    if (!hundredths) return std::nullopt;
    return Qty{*hundredths};
}

std::optional<Qty> parse_qty_round_down(std::string_view contracts) {
    const auto hundredths = parse_scaled(contracts, 2, /*allow_negative=*/true, /*round_down=*/true);
    if (!hundredths) return std::nullopt;
    return Qty{*hundredths};
}

std::string to_string(Price p) { return format_scaled(p.ticks, 4); }
std::string to_string(Qty q) { return format_scaled(q.hundredths, 2); }

void copy_cstring(char* destination, size_t destination_size, const char* source) {
    if (destination_size == 0) return;
    const size_t length = std::min(std::strlen(source), destination_size - 1);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

const char* to_string(Side s) { return s == Side::Buy ? "buy" : "sell"; }

const char* to_string(Venue v) {
    switch (v) {
        case Venue::Kalshi: return "kalshi";
        case Venue::Polymarket: return "polymarket";
        case Venue::Paper: return "paper";
        case Venue::Fix: return "fix";
    }
    return "?";
}

bool is_on_tick_grid(Price p, const std::vector<PriceRange>& ranges) {
    return is_on_tick_grid(p, ranges.data(), ranges.size());
}

bool is_on_tick_grid(Price p, const PriceRange* ranges, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const PriceRange& range = ranges[i];
        if (p < range.start || p > range.end || range.step.ticks <= 0) continue;
        if ((p.ticks - range.start.ticks) % range.step.ticks == 0) return true;
    }
    return false;
}

std::vector<PriceRange> one_cent_grid() {
    return {PriceRange{Price{100}, Price{9900}, Price{100}}};
}

}  // namespace mt
