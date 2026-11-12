#pragma once
// MarketOrderBook: price level -> total quantity for one binary contract. Not thread-safe (one owner thread).
// Prices are bounded (10'001 ticks), so each side is a flat array indexed by price plus a nonempty bitmap:
// O(1) updates, cached best, and a 64-prices-per-word bitmap scan when the best level empties. ~80 KB per side.

#include <array>
#include <cstdint>
#include <optional>

#include "core/types.hpp"

namespace mt {

struct BookTop {
    Price price;
    Qty qty;
};

class MarketOrderBook {
public:
    static constexpr int kPriceLevels = kTicksPerDollar + 1;  // 0 .. 10'000 inclusive

    MarketOrderBook() = default;

    void clear();

    // Sets the size at `price` to `qty`. False if price is outside [0, 1] or qty < 0.
    bool set_level(Side side, Price price, Qty qty);

    // Adds `delta` at `price`. False if the level would go negative: out of sync, rebuild from a snapshot.
    bool apply_delta(Side side, Price price, Qty delta);

    std::optional<BookTop> best_bid() const;
    std::optional<BookTop> best_ask() const;
    Qty qty_at(Side side, Price price) const;
    int level_count(Side side) const;

    // Bid >= ask can't rest on a real exchange, so a crossed book means we missed a message.
    bool is_crossed() const;

    // Order-independent hash of every level, for replay checks and comparing recovered books.
    uint64_t state_hash() const;

private:
    struct BookSide {
        std::array<int64_t, kPriceLevels> qty{};
        std::array<uint64_t, (kPriceLevels + 63) / 64> nonempty{};
        int level_count = 0;
        int best = -1;  // index of the best level, -1 if the side is empty
    };

    static bool in_range(Price price) { return price.ticks >= 0 && price.ticks < kPriceLevels; }
    BookSide& side_of(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const BookSide& side_of(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    void write_level(BookSide& book_side, bool is_bid, int index, int64_t qty);
    static int highest_set_at_or_below(const BookSide& book_side, int index);
    static int lowest_set_at_or_above(const BookSide& book_side, int index);

    BookSide bids_;
    BookSide asks_;
};

}  // namespace mt
