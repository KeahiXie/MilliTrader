#pragma once
// PositionKeeper: per-instrument position in YES contracts (short = holding NO), PnL in micro-dollars.
// Average cost: net = realized + (qty * mark - open_cost) - fees. A reducing fill removes a pro-rata
// share of open_cost; the integer remainder is realized when the position goes flat.

#include <array>
#include <cstdint>

#include "core/types.hpp"

namespace mt {

struct Position {
    int64_t qty = 0;              // hundredths of a YES contract, signed
    MicroDollars open_cost = 0;   // negative for a short (proceeds)
    MicroDollars realized = 0;
    MicroDollars fees = 0;
    int64_t volume = 0;           // hundredths traded, both sides (for the volume cap)
    Price mark{};                 // last mid used for unrealized PnL
    bool has_mark = false;

    MicroDollars unrealized() const;
    MicroDollars net_pnl() const { return realized + unrealized() - fees; }
};

class PositionKeeper {
public:
    void on_fill(InstrumentId instrument, Side side, Price price, Qty qty, MicroDollars fee);
    void on_mark(InstrumentId instrument, Price mid);  // called when the book's mid changes

    const Position& position(InstrumentId instrument) const { return positions_[instrument]; }
    MicroDollars total_net_pnl() const;
    int64_t total_volume() const;
    uint64_t state_hash() const;

private:
    std::array<Position, kMaxInstruments> positions_{};  // indexed by InstrumentId: no lookup, no allocation
};

}  // namespace mt
