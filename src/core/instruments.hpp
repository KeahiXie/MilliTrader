#pragma once
// InstrumentTable: maps venue tickers to dense ids. One writer (feed thread), many readers.
// Append-only: writer fills the entry then release-stores count_; readers acquire count_, no lock.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.hpp"

namespace mt {

struct InstrumentInfo {
    std::array<char, 64> symbol{};        // venue ticker / token id, NUL-terminated
    Venue venue = Venue::Kalshi;
    uint8_t tick_range_count = 0;
    std::array<PriceRange, 4> tick_ranges{};
    int64_t open_time_ns = 0;             // when the market starts trading (0 = unknown)
    int64_t close_time_ns = 0;            // when the market stops trading (0 = unknown)

    std::string_view symbol_view() const { return std::string_view(symbol.data()); }
    std::vector<PriceRange> tick_grid() const {
        return std::vector<PriceRange>(tick_ranges.begin(), tick_ranges.begin() + tick_range_count);
    }
};

InstrumentInfo make_instrument(Venue venue, std::string_view symbol,
                               const std::vector<PriceRange>& tick_grid, int64_t close_time_ns,
                               int64_t open_time_ns = 0);

class InstrumentTable {
public:
    // Writer only. Returns the existing id if known, kNoInstrument if full.
    InstrumentId add(const InstrumentInfo& info);

    // Readers. nullptr if the id is not published yet.
    const InstrumentInfo* find(InstrumentId id) const;
    InstrumentId find_id(Venue venue, std::string_view symbol) const;  // linear scan; edge only
    InstrumentId size() const { return count_.load(std::memory_order_acquire); }

private:
    std::array<InstrumentInfo, kMaxInstruments> entries_{};
    std::atomic<InstrumentId> count_{0};
};

}  // namespace mt
