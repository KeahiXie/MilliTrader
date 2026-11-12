#include "core/instruments.hpp"

#include <algorithm>
#include <cstring>

namespace mt {

InstrumentInfo make_instrument(Venue venue, std::string_view symbol,
                               const std::vector<PriceRange>& tick_grid, int64_t close_time_ns,
                               int64_t open_time_ns) {
    InstrumentInfo info;
    info.venue = venue;
    const size_t length = std::min(symbol.size(), info.symbol.size() - 1);
    std::memcpy(info.symbol.data(), symbol.data(), length);
    info.symbol[length] = '\0';
    info.tick_range_count = static_cast<uint8_t>(std::min(tick_grid.size(), info.tick_ranges.size()));
    for (size_t i = 0; i < info.tick_range_count; ++i) info.tick_ranges[i] = tick_grid[i];
    info.close_time_ns = close_time_ns;
    info.open_time_ns = open_time_ns;
    return info;
}

InstrumentId InstrumentTable::add(const InstrumentInfo& info) {
    const InstrumentId existing = find_id(info.venue, info.symbol_view());
    if (existing != kNoInstrument) return existing;

    // single writer, so relaxed is fine for our own counter
    const InstrumentId next = count_.load(std::memory_order_relaxed);
    if (next >= kMaxInstruments) return kNoInstrument;

    entries_[next] = info;
    count_.store(next + 1, std::memory_order_release);    // release: readers see the entry before the count
    return next;
}

const InstrumentInfo* InstrumentTable::find(InstrumentId id) const {
    if (id >= count_.load(std::memory_order_acquire)) return nullptr;
    return &entries_[id];
}

InstrumentId InstrumentTable::find_id(Venue venue, std::string_view symbol) const {
    const InstrumentId published = count_.load(std::memory_order_acquire);
    for (InstrumentId id = 0; id < published; ++id) {
        if (entries_[id].venue == venue && entries_[id].symbol_view() == symbol) return id;
    }
    return kNoInstrument;
}

}  // namespace mt
