#pragma once
// TradeEngine: single-threaded core (books -> strategy -> risk -> OMS -> positions), no locks.
// Deterministic for replay: "now" is the event's timestamp (timeouts are Timer events), no randomness.
// Books are allocated and the order vector reserved up front, so handling an event never mallocs.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/events.hpp"
#include "core/instruments.hpp"
#include "core/status.hpp"
#include "engine/strategy.hpp"
#include "md/market_order_book.hpp"
#include "oms/order_manager.hpp"
#include "oms/position_keeper.hpp"
#include "oms/risk_manager.hpp"

namespace mt {

// Bump when a change can alter decisions for the same inputs; replay doesn't count
// differences against another version's journal as mismatches.
//   1 = original (MTJRNL01 journals), 2 = + 250 ms back-off after risk and venue rejects
inline constexpr uint32_t kEngineLogicVersion = 2;

// Everything that shapes decisions; journaled first so replay uses the same settings.
struct EngineConfig {
    uint32_t session_id = 1;
    int64_t request_timeout_ns = 5'000'000'000;           // no answer in 5 s -> Unknown -> reconcile
    int64_t stop_quoting_before_close_ns = 60'000'000'000;  // pull quotes a minute before expiry
    Venue trading_venue = Venue::Kalshi;                  // which instruments the strategy may trade
    RiskLimits risk;
    StrategyConfig strategy;
};

class OrderSink {
public:
    virtual ~OrderSink() = default;
    virtual void send_order(const OrderRequest& request) = 0;
    virtual void on_risk_reject(const OrderRequest& request, RiskCheckResult reason) = 0;
};

class TradeEngine : private StrategyActions {
public:
    TradeEngine(const EngineConfig& config, const InstrumentTable& instruments, OrderSink& sink);

    void on_feed_event(const FeedEvent& event);
    void on_gateway_event(const GatewayEvent& event);
    void on_control_event(const ControlEvent& event);

    uint64_t state_hash() const;
    void fill_status(StatusSnapshot* status) const;

    int64_t now_ns() const { return now_ns_; }
    bool kill_switch() const { return kill_switch_; }
    bool shutdown_requested() const { return shutdown_requested_; }
    int64_t orders_sent() const { return orders_sent_; }
    int64_t fills() const { return fills_; }
    int64_t feed_events() const { return feed_events_; }
    int64_t cancels_throttled() const { return cancels_throttled_; }
    int64_t placements_suppressed() const { return placements_suppressed_; }
    const OrderManager& orders() const { return oms_; }
    const PositionKeeper& positions() const { return positions_; }
    const RiskManager& risk() const { return risk_; }
    const MarketOrderBook& book(InstrumentId id) const { return books_[id]->book; }

private:
    struct BookState {
        MarketOrderBook book;
        bool valid = false;        // a full snapshot has been applied and no gap is pending
        int64_t last_update_ns = 0;
        OrderId bid_quote = 0;     // the strategy's resting quotes on this instrument
        OrderId ask_quote = 0;
    };

    // StrategyActions: the strategy's only way to act.
    OrderId place(InstrumentId instrument, Side side, Price price, Qty qty) override;
    void cancel(OrderId id) override;

    void advance_clock(int64_t event_ns);
    bool is_tradable(InstrumentId instrument) const;
    void run_strategy(InstrumentId instrument);
    QuoteState quote_state(OrderId& slot);
    void cancel_all_open_orders();
    void cancel_quotes_on(InstrumentId instrument);
    void send(const OrderRequest& request);

    EngineConfig config_;
    const InstrumentTable& instruments_;
    OrderSink& sink_;

    std::array<std::unique_ptr<BookState>, kMaxInstruments> books_;
    OrderManager oms_;
    RiskManager risk_;
    PositionKeeper positions_;
    JoinBestStrategy strategy_;

    int64_t now_ns_ = 0;
    OrderId next_order_id_ = 1;
    bool kill_switch_ = false;
    bool shutdown_requested_ = false;
    int64_t orders_sent_ = 0;
    int64_t fills_ = 0;
    int64_t feed_events_ = 0;
    // Cancel-storm guard: token bucket in event time, at most kMaxCancelsPerSecond cancels.
    static constexpr int kMaxCancelsPerSecond = 20;
    int64_t cancel_tokens_milli_ = kMaxCancelsPerSecond * 1000;
    int64_t cancel_tokens_refilled_ns_ = 0;
    int64_t cancels_throttled_ = 0;
    // After a reject on (instrument, side), wait kRejectBackoffNs before placing again,
    // or a fast-updating book turns one reject into thousands.
    static constexpr int64_t kRejectBackoffNs = 250'000'000;
    std::array<std::array<int64_t, 2>, kMaxInstruments> last_reject_ns_{};
    int64_t placements_suppressed_ = 0;
    std::vector<OrderId> scratch_ids_;  // reused for timer results: no allocation per tick
};

}  // namespace mt
