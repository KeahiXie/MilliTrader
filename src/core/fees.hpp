#pragma once
// Taker fees in integer micro-dollars: 0.07 * C * p * (1 - p), rounded up. No maker fees here.
// Kalshi rounds to the cent; Polymarket to $0.00001 (direction undocumented, so we round up).
// The product overflows int64 for big orders, so the intermediate is 128-bit.

#include <cstdint>

#include "core/types.hpp"

namespace mt {

// __extension__ keeps -Wpedantic quiet about __int128.
__extension__ using Int128 = __int128;

// rate in parts per million: 0.07 -> 70'000.
inline constexpr int64_t kKalshiTakerRatePpm = 70'000;
inline constexpr int64_t kKalshiMakerRatePpm = 17'500;  // only on "quadratic_with_maker_fees" series
inline constexpr int64_t kPolymarketCryptoRatePpm = 70'000;

// Fee rounded up to a multiple of round_to_micro.
// fee micro = rate_ppm * qty * p * (1e4 - p) / 1e10  (qty in hundredths, p in $0.0001 ticks)
inline MicroDollars quadratic_fee(Price price, Qty qty, int64_t rate_ppm, int64_t round_to_micro) {
    if (qty.hundredths <= 0 || price.ticks <= 0 || price.ticks >= kTicksPerDollar) return 0;
    const Int128 numerator = static_cast<Int128>(rate_ppm) * qty.hundredths * price.ticks *
                               (kTicksPerDollar - price.ticks);
    const Int128 denominator = static_cast<Int128>(10'000'000'000LL) * round_to_micro;
    const Int128 units = (numerator + denominator - 1) / denominator;  // ceiling division
    return static_cast<MicroDollars>(units * round_to_micro);
}

inline MicroDollars kalshi_taker_fee(Price price, Qty qty) {
    return quadratic_fee(price, qty, kKalshiTakerRatePpm, /*round to $0.01*/ 10'000);
}

inline MicroDollars polymarket_taker_fee(Price price, Qty qty) {
    return quadratic_fee(price, qty, kPolymarketCryptoRatePpm, /*round to $0.00001*/ 10);
}

}  // namespace mt
