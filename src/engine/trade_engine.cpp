#include "engine/trade_engine.hpp"

#include <algorithm>
#include <cstring>

#include "core/fees.hpp"

namespace mt {

TradeEngine::TradeEngine(const EngineConfig& config, const InstrumentTable& instruments, OrderSink& sink)
    : config_(config),
      instruments_(instruments),
      sink_(sink),
      oms_(config.request_timeout_ns),
      risk_(config.risk),
      strategy_(config.strategy) {
    for (auto& slot : books_) slot = std::make_unique<BookState>();
    scratch_ids_.reserve(1024);
}

void TradeEngine::advance_clock(int64_t event_ns) {
    // stamps from different queues can be slightly out of order; the clock only moves forward
    now_ns_ = std::max(now_ns_, event_ns);
}

bool TradeEngine::is_tradable(InstrumentId instrument) const {
    const InstrumentInfo* info = instruments_.find(instrument);
    if (info == nullptr || info->venue != config_.trading_venue) return false;
    if (info->open_time_ns != 0 && now_ns_ < info->open_time_ns) return false;
    if (info->close_time_ns != 0 && now_ns_ >= info->close_time_ns - config_.stop_quoting_before_close_ns) return false;
    return books_[instrument]->valid;
}

void TradeEngine::on_feed_event(const FeedEvent& event) {
    if (event.instrument >= kMaxInstruments) return;
    advance_clock(event.receive_ns);
    ++feed_events_;
    BookState& state = *books_[event.instrument];
    state.last_update_ns = event.receive_ns;

    switch (event.type) {
        case FeedEventType::BookClear:
            state.book.clear();
            state.valid = false;
            return;  // wait for the rest of the snapshot
        case FeedEventType::BookLevel:
            state.book.set_level(event.side, event.price, event.qty);
            return;
        case FeedEventType::BookSnapshotDone:
            state.valid = true;
            break;
        case FeedEventType::BookDelta:
            if (!state.book.apply_delta(event.side, event.price, event.qty)) state.valid = false;
            break;
        case FeedEventType::BookStale:
            state.valid = false;
            break;
        case FeedEventType::MarketClosed:
            cancel_quotes_on(event.instrument);
            return;
        default:
            return;
    }

    if (state.valid) {
        const auto bid = state.book.best_bid();
        const auto ask = state.book.best_ask();
        if (bid && ask) positions_.on_mark(event.instrument, Price{(bid->price.ticks + ask->price.ticks) / 2});
    }
    run_strategy(event.instrument);
}

void TradeEngine::on_gateway_event(const GatewayEvent& event) {
    advance_clock(event.receive_ns);
    const Order* order = oms_.find(event.order_id);
    const OrderUpdate update = oms_.on_gateway_event(event, now_ns_);
    if (!update.known || order == nullptr) return;

    // venue reject: back off that side too, or the strategy re-sends the same doomed order
    if (update.new_state == OrderState::Rejected && update.old_state != OrderState::Rejected &&
        order->instrument < kMaxInstruments) {
        last_reject_ns_[order->instrument][order->side == Side::Buy ? 0 : 1] = now_ns_;
    }

    if (update.fill_qty.hundredths > 0) {
        ++fills_;
        // venue-reported fee if present, else the model (KXBTC15M charges takers only)
        const MicroDollars fee = event.fee != 0 ? event.fee
                                 : update.is_taker ? kalshi_taker_fee(update.fill_price, update.fill_qty)
                                                   : 0;
        positions_.on_fill(order->instrument, order->side, update.fill_price, update.fill_qty, fee);
    }
    // A loss beyond the limit trips the kill switch by itself.
    if (positions_.total_net_pnl() < -config_.risk.max_loss && !kill_switch_) {
        kill_switch_ = true;
        cancel_all_open_orders();
    }
    run_strategy(order->instrument);
}

void TradeEngine::on_control_event(const ControlEvent& event) {
    advance_clock(event.now_ns);
    switch (event.type) {
        case ControlEventType::Timer: {
            scratch_ids_.clear();
            oms_.on_timer(now_ns_, &scratch_ids_);
            // timed-out requests: never resend, reconcile by client order id instead
            for (const OrderId id : scratch_ids_) {
                const Order* order = oms_.find(id);
                OrderRequest request;
                request.type = OrderRequestType::Reconcile;
                request.order_id = id;
                request.instrument = order->instrument;
                request.side = order->side;
                send(request);
            }
            // Pull quotes from markets that are about to close.
            for (InstrumentId id = 0; id < instruments_.size(); ++id) {
                if (!is_tradable(id) && (books_[id]->bid_quote != 0 || books_[id]->ask_quote != 0)) cancel_quotes_on(id);
            }
            break;
        }
        case ControlEventType::KillSwitchOn:
            kill_switch_ = true;
            cancel_all_open_orders();
            break;
        case ControlEventType::KillSwitchOff:
            kill_switch_ = false;
            break;
        case ControlEventType::SetMaxOrderQty: {
            RiskLimits limits = risk_.limits();
            limits.max_order_qty = Qty{event.value};
            risk_.set_limits(limits);
            break;
        }
        case ControlEventType::SetMaxPosition: {
            RiskLimits limits = risk_.limits();
            limits.max_position = Qty{event.value};
            risk_.set_limits(limits);
            break;
        }
        case ControlEventType::Shutdown:
            shutdown_requested_ = true;
            kill_switch_ = true;
            cancel_all_open_orders();
            break;
    }
}

QuoteState TradeEngine::quote_state(OrderId& slot) {
    QuoteState quote;
    if (slot == 0) return quote;
    const Order* order = oms_.find(slot);
    if (order == nullptr || is_final(order->state)) {
        slot = 0;  // gone: the strategy may quote this side again
        return quote;
    }
    quote.order_id = slot;
    quote.price = order->price;
    quote.cancelable = order->state == OrderState::New || order->state == OrderState::PartiallyFilled;
    return quote;
}

void TradeEngine::run_strategy(InstrumentId instrument) {
    const InstrumentInfo* info = instruments_.find(instrument);
    if (info == nullptr || info->venue != config_.trading_venue) return;
    BookState& state = *books_[instrument];
    const QuoteState bid = quote_state(state.bid_quote);
    const QuoteState ask = quote_state(state.ask_quote);
    const bool tradable = is_tradable(instrument) && !kill_switch_;
    // Nothing to do on a market we neither quote nor may trade.
    if (!tradable && bid.order_id == 0 && ask.order_id == 0) return;
    strategy_.on_book(instrument, state.book, tradable, positions_.position(instrument).qty, bid, ask, *this);
}

OrderId TradeEngine::place(InstrumentId instrument, Side side, Price price, Qty qty) {
    int64_t& last_reject = last_reject_ns_[instrument][side == Side::Buy ? 0 : 1];
    if (last_reject != 0 && now_ns_ - last_reject < kRejectBackoffNs) {
        ++placements_suppressed_;
        return 0;
    }
    OrderRequest request;
    request.type = OrderRequestType::New;
    request.instrument = instrument;
    request.side = side;
    request.price = price;
    request.qty = qty;
    request.post_only = true;  // a joining quote must never take liquidity by accident

    const InstrumentInfo* info = instruments_.find(instrument);
    const BookState& state = *books_[instrument];

    RiskContext context;
    context.kill_switch = kill_switch_;
    context.market_open = is_tradable(instrument);
    context.book_fresh = state.valid;
    if (info != nullptr) {
        context.tick_ranges = info->tick_ranges.data();
        context.tick_range_count = info->tick_range_count;
    }
    if (const auto bid = state.book.best_bid()) {
        context.has_bid = true;
        context.best_bid = bid->price;
    }
    if (const auto ask = state.book.best_ask()) {
        context.has_ask = true;
        context.best_ask = ask->price;
    }
    context.position = positions_.position(instrument).qty;
    context.open_buy_qty = oms_.open_qty(instrument, Side::Buy);
    context.open_sell_qty = oms_.open_qty(instrument, Side::Sell);
    context.open_orders = oms_.open_order_count();
    context.net_pnl = positions_.total_net_pnl();
    context.traded_volume = positions_.total_volume();
    context.now_ns = now_ns_;

    const RiskCheckResult result = risk_.check_new_order(request, context);
    if (result != RiskCheckResult::Allowed) {
        sink_.on_risk_reject(request, result);
        last_reject = now_ns_;
        return 0;
    }

    request.order_id = next_order_id_++;
    oms_.create(request.order_id, instrument, side, price, qty, now_ns_);  // OMS state before the send, so the response always finds the order
    BookState& mutable_state = *books_[instrument];
    (side == Side::Buy ? mutable_state.bid_quote : mutable_state.ask_quote) = request.order_id;
    send(request);
    return request.order_id;
}

void TradeEngine::cancel(OrderId id) {
    const Order* order = oms_.find(id);
    if (order == nullptr) return;
    // Token bucket, integer milli-tokens so replay is exact.
    if (cancel_tokens_refilled_ns_ == 0) cancel_tokens_refilled_ns_ = now_ns_;
    const int64_t refill = (now_ns_ - cancel_tokens_refilled_ns_) * kMaxCancelsPerSecond / 1'000'000;  // milli-tokens
    cancel_tokens_milli_ = std::min<int64_t>(kMaxCancelsPerSecond * 1000, cancel_tokens_milli_ + refill);
    cancel_tokens_refilled_ns_ = now_ns_;
    if (cancel_tokens_milli_ < 1000) {
        ++cancels_throttled_;
        return;  // the strategy asks again on a later event
    }
    if (!oms_.request_cancel(id, now_ns_)) return;
    cancel_tokens_milli_ -= 1000;
    OrderRequest request;
    request.type = OrderRequestType::Cancel;
    request.order_id = id;
    request.instrument = order->instrument;
    request.side = order->side;
    request.price = order->price;
    request.qty = order->qty;
    send(request);
}

void TradeEngine::cancel_quotes_on(InstrumentId instrument) {
    BookState& state = *books_[instrument];
    if (state.bid_quote != 0) cancel(state.bid_quote);
    if (state.ask_quote != 0) cancel(state.ask_quote);
}

void TradeEngine::cancel_all_open_orders() {
    for (const Order& order : oms_.all_orders()) {
        if (order.state == OrderState::New || order.state == OrderState::PartiallyFilled) cancel(order.id);
    }
}

void TradeEngine::send(const OrderRequest& request) {
    if (request.type == OrderRequestType::New) ++orders_sent_;
    sink_.send_order(request);
}

uint64_t TradeEngine::state_hash() const {
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (InstrumentId id = 0; id < kMaxInstruments; ++id) {
        const BookState& state = *books_[id];
        mix(state.valid ? state.book.state_hash() : 0);
        mix(state.bid_quote);
        mix(state.ask_quote);
    }
    mix(oms_.state_hash());
    mix(positions_.state_hash());
    mix(next_order_id_);
    mix(kill_switch_ ? 1 : 0);
    mix(static_cast<uint64_t>(risk_.total_rejects()));
    mix(static_cast<uint64_t>(cancels_throttled_));
    mix(static_cast<uint64_t>(placements_suppressed_));
    return hash;
}

void TradeEngine::fill_status(StatusSnapshot* status) const {
    status->engine_time_ns = now_ns_;
    status->kill_switch = kill_switch_;
    status->feed_events = feed_events_;
    status->orders_sent = orders_sent_;
    status->fills = fills_;
    status->risk_rejects = risk_.total_rejects();
    status->unknown_orders = oms_.unknown_count();
    status->total_pnl = positions_.total_net_pnl();

    // The newest instruments are the live ones (markets roll every 15 min).
    status->instrument_count = 0;
    const InstrumentId count = instruments_.size();
    for (InstrumentId offset = 0; offset < count && status->instrument_count < 4; ++offset) {
        const InstrumentId id = count - 1 - offset;
        const InstrumentInfo* info = instruments_.find(id);
        const BookState& state = *books_[id];
        if (info == nullptr || (!state.valid && positions_.position(id).qty == 0)) continue;
        InstrumentStatus& out = status->instruments[status->instrument_count++];
        copy_cstring(out.symbol, sizeof(out.symbol), info->symbol.data());
        out.tradable = is_tradable(id);
        if (const auto bid = state.book.best_bid()) {
            out.has_bid = true;
            out.bid = bid->price;
            out.bid_qty = bid->qty;
        }
        if (const auto ask = state.book.best_ask()) {
            out.has_ask = true;
            out.ask = ask->price;
            out.ask_qty = ask->qty;
        }
        const Position& p = positions_.position(id);
        out.position = p.qty;
        out.realized = p.realized;
        out.unrealized = p.unrealized();
        out.fees = p.fees;
    }

    status->open_order_count = 0;
    const auto& orders = oms_.all_orders();
    for (auto it = orders.rbegin(); it != orders.rend() && status->open_order_count < 8; ++it) {
        if (is_final(it->state)) continue;
        OpenOrderStatus& out = status->open_orders[status->open_order_count++];
        out.order_id = it->id;
        out.instrument = it->instrument;
        out.side = it->side;
        out.state = static_cast<uint8_t>(it->state);
        out.price = it->price;
        out.qty = it->qty;
        out.filled = it->filled;
    }
}

}  // namespace mt
