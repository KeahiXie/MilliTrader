#pragma once
// Basic value types. Prices, quantities and money are fixed-point integers parsed straight
// from venue decimal strings, never doubles (one tick off is a reject or a fake signal).

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mt {

// Price: integer $0.0001 ticks ("0.5600" -> 5600). Contracts settle at $0 or $1 (10'000 ticks).
inline constexpr int32_t kTicksPerDollar = 10'000;

struct Price {
    int32_t ticks = 0;
    constexpr auto operator<=>(const Price&) const = default;
};

inline constexpr Price kMinPrice{0};
inline constexpr Price kMaxPrice{kTicksPerDollar};

// Other side of a binary contract: a NO bid at $0.30 is a YES ask at $0.70.
constexpr Price complement(Price p) { return Price{kTicksPerDollar - p.ticks}; }

// Qty: hundredths of a contract ("12.50" -> 1250). Both venues allow 0.01 granularity.
inline constexpr int64_t kQtyPerContract = 100;

struct Qty {
    int64_t hundredths = 0;
    constexpr auto operator<=>(const Qty&) const = default;
    constexpr Qty& operator+=(Qty other) { hundredths += other.hundredths; return *this; }
    constexpr Qty& operator-=(Qty other) { hundredths -= other.hundredths; return *this; }
};

constexpr Qty operator+(Qty a, Qty b) { return Qty{a.hundredths + b.hundredths}; }
constexpr Qty operator-(Qty a, Qty b) { return Qty{a.hundredths - b.hundredths}; }
constexpr Qty contracts(int64_t whole_contracts) { return Qty{whole_contracts * kQtyPerContract}; }

// Money: micro-dollars. ticks (1e-4) x hundredths (1e-2) = 1e-6, so price * qty is exact.
using MicroDollars = int64_t;

constexpr MicroDollars notional(Price p, Qty q) {
    return static_cast<MicroDollars>(p.ticks) * q.hundredths;
}

enum class Side : uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class Venue : uint8_t { Kalshi, Polymarket, Paper, Fix };

// Dense id assigned by the feed thread, so the engine indexes arrays instead of comparing tickers.
using InstrumentId = uint16_t;
inline constexpr InstrumentId kMaxInstruments = 64;
inline constexpr InstrumentId kNoInstrument = 0xFFFF;

// Our order id. client_order_id ("mt-<session>-<id>") is built from it, so it's known before the send.
using OrderId = uint64_t;

// Parse plain decimals ("0.56", "12.5"). Rejects signs (except "-" for qty), extra decimals, junk.
std::optional<Price> parse_price(std::string_view dollars);
std::optional<Qty> parse_qty(std::string_view contracts);
// Drops extra decimals instead of rejecting ("20.3125" -> 20.31). Display-only sizes.
std::optional<Qty> parse_qty_round_down(std::string_view contracts);

std::string to_string(Price p);  // 5600 -> "0.5600"
std::string to_string(Qty q);    // 1250 -> "12.50", -50 -> "-0.50"
const char* to_string(Side s);

// Copy into a fixed char buffer, always NUL-terminated, truncating if needed.
void copy_cstring(char* destination, size_t destination_size, const char* source);
const char* to_string(Venue v);

// Tick grid from Kalshi "price_ranges", e.g. [0.00, 0.10] step 0.001, [0.10, 0.90] step 0.01.
// A price is valid if some range contains it and it sits on that range's step.
struct PriceRange {
    Price start;
    Price end;
    Price step;
};

bool is_on_tick_grid(Price p, const std::vector<PriceRange>& ranges);
bool is_on_tick_grid(Price p, const PriceRange* ranges, size_t count);  // no-allocation form

// 1-cent grid from $0.01 to $0.99 (most Kalshi markets).
std::vector<PriceRange> one_cent_grid();

}  // namespace mt
