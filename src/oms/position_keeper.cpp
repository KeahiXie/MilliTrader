#include "oms/position_keeper.hpp"

#include <algorithm>
#include <cstdlib>

namespace mt {

MicroDollars Position::unrealized() const {
    if (!has_mark || qty == 0) return 0;
    return static_cast<MicroDollars>(mark.ticks) * qty - open_cost;
}

void PositionKeeper::on_fill(InstrumentId instrument, Side side, Price price, Qty qty, MicroDollars fee) {
    if (instrument >= kMaxInstruments || qty.hundredths <= 0) return;
    Position& p = positions_[instrument];
    p.fees += fee;
    p.volume += qty.hundredths;

    // signed fill size: buy adds, sell subtracts
    int64_t remaining = side == Side::Buy ? qty.hundredths : -qty.hundredths;
    const MicroDollars price_ticks = price.ticks;

    // Part of the fill that reduces the open position.
    if (p.qty != 0 && (p.qty > 0) != (remaining > 0)) {
        const int64_t closing = std::min(std::llabs(p.qty), std::llabs(remaining));
        const MicroDollars cost_removed = p.open_cost * closing / std::llabs(p.qty);
        if (p.qty > 0) {
            p.realized += price_ticks * closing - cost_removed;    // sold for more than it cost?
            p.qty -= closing;
            remaining += closing;
        } else {
            p.realized += -cost_removed - price_ticks * closing;   // bought back for less than we sold?
            p.qty += closing;
            remaining -= closing;
        }
        p.open_cost -= cost_removed;
        if (p.qty == 0) {
            p.realized -= p.open_cost;  // rounding remainder, if any
            p.open_cost = 0;
        }
    }
    // Whatever is left opens or adds to a position in the fill's direction.
    if (remaining != 0) {
        p.qty += remaining;
        p.open_cost += price_ticks * remaining;
    }
}

void PositionKeeper::on_mark(InstrumentId instrument, Price mid) {
    if (instrument >= kMaxInstruments) return;
    positions_[instrument].mark = mid;
    positions_[instrument].has_mark = true;
}

MicroDollars PositionKeeper::total_net_pnl() const {
    MicroDollars total = 0;
    for (const Position& p : positions_) total += p.net_pnl();
    return total;
}

int64_t PositionKeeper::total_volume() const {
    int64_t total = 0;
    for (const Position& p : positions_) total += p.volume;
    return total;
}

uint64_t PositionKeeper::state_hash() const {
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (const Position& p : positions_) {
        mix(static_cast<uint64_t>(p.qty));
        mix(static_cast<uint64_t>(p.open_cost));
        mix(static_cast<uint64_t>(p.realized));
        mix(static_cast<uint64_t>(p.fees));
    }
    return hash;
}

}  // namespace mt
