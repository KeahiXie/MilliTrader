#include "md/market_order_book.hpp"

namespace mt {

void MarketOrderBook::clear() {
    bids_ = BookSide{};
    asks_ = BookSide{};
}

bool MarketOrderBook::set_level(Side side, Price price, Qty qty) {
    if (!in_range(price) || qty.hundredths < 0) return false;
    write_level(side_of(side), side == Side::Buy, price.ticks, qty.hundredths);
    return true;
}

bool MarketOrderBook::apply_delta(Side side, Price price, Qty delta) {
    if (!in_range(price)) return false;
    BookSide& book_side = side_of(side);
    const int64_t updated = book_side.qty[price.ticks] + delta.hundredths;
    if (updated < 0) return false;
    write_level(book_side, side == Side::Buy, price.ticks, updated);
    return true;
}

void MarketOrderBook::write_level(BookSide& book_side, bool is_bid, int index, int64_t qty) {
    const bool was_set = book_side.qty[index] > 0;
    const bool now_set = qty > 0;
    book_side.qty[index] = qty;
    if (was_set == now_set) return;  // size changed but the level still exists (or still doesn't)

    const uint64_t bit = uint64_t{1} << (index % 64);
    if (now_set) {
        book_side.nonempty[index / 64] |= bit;
        book_side.level_count += 1;
        // A new level can only improve the best price.
        if (book_side.best < 0 || (is_bid ? index > book_side.best : index < book_side.best)) book_side.best = index;
    } else {
        book_side.nonempty[index / 64] &= ~bit;
        book_side.level_count -= 1;
        // Only removing the best level forces a search for the next one.
        if (index == book_side.best) {
            book_side.best = is_bid ? highest_set_at_or_below(book_side, index)
                                    : lowest_set_at_or_above(book_side, index);
        }
    }
}

int MarketOrderBook::highest_set_at_or_below(const BookSide& book_side, int index) {
    int word = index / 64;
    // Mask off bits above `index` in its own word, then walk down whole words.
    const int bit = index % 64;
    uint64_t bits = book_side.nonempty[word] & (bit == 63 ? ~uint64_t{0} : ((uint64_t{1} << (bit + 1)) - 1));
    while (true) {
        if (bits != 0) return word * 64 + (63 - __builtin_clzll(bits));
        if (--word < 0) return -1;
        bits = book_side.nonempty[word];
    }
}

int MarketOrderBook::lowest_set_at_or_above(const BookSide& book_side, int index) {
    int word = index / 64;
    const int bit = index % 64;
    uint64_t bits = book_side.nonempty[word] & (~uint64_t{0} << bit);
    const int last_word = static_cast<int>(book_side.nonempty.size()) - 1;
    while (true) {
        if (bits != 0) return word * 64 + __builtin_ctzll(bits);
        if (++word > last_word) return -1;
        bits = book_side.nonempty[word];
    }
}

std::optional<BookTop> MarketOrderBook::best_bid() const {
    if (bids_.best < 0) return std::nullopt;
    return BookTop{Price{bids_.best}, Qty{bids_.qty[bids_.best]}};
}

std::optional<BookTop> MarketOrderBook::best_ask() const {
    if (asks_.best < 0) return std::nullopt;
    return BookTop{Price{asks_.best}, Qty{asks_.qty[asks_.best]}};
}

Qty MarketOrderBook::qty_at(Side side, Price price) const {
    if (!in_range(price)) return Qty{};
    return Qty{side_of(side).qty[price.ticks]};
}

int MarketOrderBook::level_count(Side side) const { return side_of(side).level_count; }

bool MarketOrderBook::is_crossed() const {
    return bids_.best >= 0 && asks_.best >= 0 && bids_.best >= asks_.best;
}

uint64_t MarketOrderBook::state_hash() const {
    // FNV-1a over (side, price, qty) of each non-empty level, walked via the bitmaps.
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xFF;
            hash *= 1099511628211ULL;
        }
    };
    for (int side = 0; side < 2; ++side) {
        const BookSide& book_side = side == 0 ? bids_ : asks_;
        for (size_t word = 0; word < book_side.nonempty.size(); ++word) {
            uint64_t bits = book_side.nonempty[word];
            while (bits != 0) {
                const int index = static_cast<int>(word * 64) + __builtin_ctzll(bits);
                mix(static_cast<uint64_t>(side) << 32 | static_cast<uint64_t>(index));
                mix(static_cast<uint64_t>(book_side.qty[index]));
                bits &= bits - 1;  // clear the lowest set bit
            }
        }
    }
    return hash;
}

}  // namespace mt
