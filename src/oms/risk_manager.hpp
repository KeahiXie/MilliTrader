#pragma once
// RiskManager: pre-trade checks for every new order, cheapest first; the first failure is the reason.
// Cancels skip it so reducing risk is never blocked. Uses event time only (incl. the rate limiter),
// so a replay makes the same decisions.

#include <cstdint>
#include <vector>

#include "core/events.hpp"
#include "core/types.hpp"

namespace mt {

enum class RiskCheckResult : uint8_t {
    Allowed,
    KillSwitch,                 // trading halted by the operator or by a limit breach
    MarketNotOpen,              // outside the market's trading hours, or the book is stale
    OrderTooLarge,              // single order above max_order_qty
    OffGridPrice,               // not on the market's tick grid
    PriceCollar,                // too far from the current touch (fat-finger guard)
    TooManyOpenOrders,
    RateLimited,                // orders per second above the limit
    PositionTooLarge,           // position after a full fill above max_position
    WorstCasePositionTooLarge,  // position + every open order on that side, all filled
    LossTooLarge,               // net PnL (after fees) below -max_loss
    VolumeCapHit,               // traded volume today above the cap
};

const char* to_string(RiskCheckResult result);

struct RiskLimits {
    Qty max_order_qty = contracts(5);
    Qty max_position = contracts(10);
    Qty max_worst_case_position = contracts(15);
    MicroDollars max_loss = 5'000'000;           // $5.00
    Qty max_daily_volume = contracts(500);
    int32_t price_collar_ticks = 1000;           // 10 cents from the touch
    int max_open_orders = 6;
    int max_orders_per_second = 5;
};

// Everything the checks need, gathered by the engine.
struct RiskContext {
    bool kill_switch = false;
    bool market_open = false;
    bool book_fresh = false;
    const PriceRange* tick_ranges = nullptr;   // the market's tick grid (points into InstrumentInfo)
    size_t tick_range_count = 0;
    bool has_bid = false, has_ask = false;
    Price best_bid{}, best_ask{};
    int64_t position = 0;       // hundredths, signed
    Qty open_buy_qty{};
    Qty open_sell_qty{};
    int open_orders = 0;
    MicroDollars net_pnl = 0;
    int64_t traded_volume = 0;  // hundredths
    int64_t now_ns = 0;
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits);

    RiskCheckResult check_new_order(const OrderRequest& order, const RiskContext& context);

    void set_limits(const RiskLimits& limits) { limits_ = limits; }
    const RiskLimits& limits() const { return limits_; }

    int64_t rejects(RiskCheckResult reason) const { return reject_counts_[static_cast<int>(reason)]; }
    int64_t total_rejects() const;

private:
    bool take_rate_token(int64_t now_ns);

    RiskLimits limits_;
    // Token bucket: refills at max_orders_per_second, holds at most that many.
    double tokens_ = 0;
    int64_t last_refill_ns_ = 0;
    int64_t reject_counts_[12] = {};
};

}  // namespace mt
