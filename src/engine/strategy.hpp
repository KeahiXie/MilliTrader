#pragma once
// JoinBestStrategy: minimal two-sided quoter to exercise the order path, not to make money.
// Joins the best bid and ask with one quote each, cancels when the touch moves, and stops
// quoting a side at max_inventory. Acts only through StrategyActions, so risk always applies.

#include <cstdint>

#include "core/types.hpp"
#include "md/market_order_book.hpp"

namespace mt {

struct StrategyConfig {
    Qty order_qty = contracts(1);
    Qty max_inventory = contracts(5);
    int32_t min_spread_ticks = 100;   // don't quote a 0- or sub-cent-wide market
    bool enabled = true;
};

class StrategyActions {
public:
    virtual ~StrategyActions() = default;
    // Returns the new order id, or 0 if risk rejected it.
    virtual OrderId place(InstrumentId instrument, Side side, Price price, Qty qty) = 0;
    virtual void cancel(OrderId id) = 0;
};

// The strategy's own quote on one side.
struct QuoteState {
    OrderId order_id = 0;      // 0 = none
    Price price{};
    bool cancelable = false;   // resting (New/PartiallyFilled), not pending
};

class JoinBestStrategy {
public:
    explicit JoinBestStrategy(StrategyConfig config) : config_(config) {}

    void on_book(InstrumentId instrument, const MarketOrderBook& book, bool tradable, int64_t position,
                 const QuoteState& bid_quote, const QuoteState& ask_quote, StrategyActions& actions);

    const StrategyConfig& config() const { return config_; }
    void set_enabled(bool enabled) { config_.enabled = enabled; }

private:
    void manage_side(InstrumentId instrument, Side side, bool want_quote, Price target, const QuoteState& quote,
                     StrategyActions& actions);
    StrategyConfig config_;
};

}  // namespace mt
