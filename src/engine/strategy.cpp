#include "engine/strategy.hpp"

namespace mt {

void JoinBestStrategy::manage_side(InstrumentId instrument, Side side, bool want_quote, Price target,
                                   const QuoteState& quote, StrategyActions& actions) {
    if (quote.order_id != 0) {
        // pull a resting quote if we no longer want it or the touch moved
        const bool stale = !want_quote || quote.price != target;
        if (stale && quote.cancelable) actions.cancel(quote.order_id);
        return;  // in-flight requests: wait for the answer before doing more
    }
    if (want_quote) actions.place(instrument, side, target, config_.order_qty);
}

void JoinBestStrategy::on_book(InstrumentId instrument, const MarketOrderBook& book, bool tradable, int64_t position,
                               const QuoteState& bid_quote, const QuoteState& ask_quote, StrategyActions& actions) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    const bool quotable = config_.enabled && tradable && bid && ask &&
                          ask->price.ticks - bid->price.ticks >= config_.min_spread_ticks;

    const bool want_bid = quotable && position + config_.order_qty.hundredths <= config_.max_inventory.hundredths;
    const bool want_ask = quotable && position - config_.order_qty.hundredths >= -config_.max_inventory.hundredths;
    manage_side(instrument, Side::Buy, want_bid, bid ? bid->price : Price{}, bid_quote, actions);
    manage_side(instrument, Side::Sell, want_ask, ask ? ask->price : Price{}, ask_quote, actions);
}

}  // namespace mt
