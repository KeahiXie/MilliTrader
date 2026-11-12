#include "oms/risk_manager.hpp"

#include <algorithm>
#include <cstdlib>

namespace mt {

const char* to_string(RiskCheckResult result) {
    switch (result) {
        case RiskCheckResult::Allowed: return "Allowed";
        case RiskCheckResult::KillSwitch: return "KillSwitch";
        case RiskCheckResult::MarketNotOpen: return "MarketNotOpen";
        case RiskCheckResult::OrderTooLarge: return "OrderTooLarge";
        case RiskCheckResult::OffGridPrice: return "OffGridPrice";
        case RiskCheckResult::PriceCollar: return "PriceCollar";
        case RiskCheckResult::TooManyOpenOrders: return "TooManyOpenOrders";
        case RiskCheckResult::RateLimited: return "RateLimited";
        case RiskCheckResult::PositionTooLarge: return "PositionTooLarge";
        case RiskCheckResult::WorstCasePositionTooLarge: return "WorstCasePositionTooLarge";
        case RiskCheckResult::LossTooLarge: return "LossTooLarge";
        case RiskCheckResult::VolumeCapHit: return "VolumeCapHit";
    }
    return "?";
}

RiskManager::RiskManager(RiskLimits limits) : limits_(limits), tokens_(limits.max_orders_per_second) {}

bool RiskManager::take_rate_token(int64_t now_ns) {
    if (last_refill_ns_ == 0) last_refill_ns_ = now_ns;
    const double elapsed_s = static_cast<double>(now_ns - last_refill_ns_) / 1e9;
    tokens_ = std::min<double>(limits_.max_orders_per_second, tokens_ + elapsed_s * limits_.max_orders_per_second);
    last_refill_ns_ = now_ns;
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
}

RiskCheckResult RiskManager::check_new_order(const OrderRequest& order, const RiskContext& context) {
    auto reject = [this](RiskCheckResult reason) {
        reject_counts_[static_cast<int>(reason)] += 1;
        return reason;
    };
    const int64_t signed_qty = order.side == Side::Buy ? order.qty.hundredths : -order.qty.hundredths;

    if (context.kill_switch) return reject(RiskCheckResult::KillSwitch);
    if (!context.market_open || !context.book_fresh) return reject(RiskCheckResult::MarketNotOpen);
    if (order.qty.hundredths <= 0 || order.qty > limits_.max_order_qty) return reject(RiskCheckResult::OrderTooLarge);
    if (context.tick_ranges == nullptr || !is_on_tick_grid(order.price, context.tick_ranges, context.tick_range_count)) {
        return reject(RiskCheckResult::OffGridPrice);
    }
    // Fat-finger collar: a buy far above the ask or a sell far below the bid is almost surely a bug.
    if (order.side == Side::Buy && context.has_ask &&
        order.price.ticks > context.best_ask.ticks + limits_.price_collar_ticks) {
        return reject(RiskCheckResult::PriceCollar);
    }
    if (order.side == Side::Sell && context.has_bid &&
        order.price.ticks < context.best_bid.ticks - limits_.price_collar_ticks) {
        return reject(RiskCheckResult::PriceCollar);
    }
    if (context.open_orders >= limits_.max_open_orders) return reject(RiskCheckResult::TooManyOpenOrders);
    if (!take_rate_token(context.now_ns)) return reject(RiskCheckResult::RateLimited);

    if (std::llabs(context.position + signed_qty) > limits_.max_position.hundredths) {
        return reject(RiskCheckResult::PositionTooLarge);
    }
    // Worst case: every open order on this side fills, plus this one.
    const int64_t worst_case = order.side == Side::Buy
                                   ? context.position + context.open_buy_qty.hundredths + order.qty.hundredths
                                   : context.position - context.open_sell_qty.hundredths - order.qty.hundredths;
    if (std::llabs(worst_case) > limits_.max_worst_case_position.hundredths) {
        return reject(RiskCheckResult::WorstCasePositionTooLarge);
    }
    if (context.net_pnl < -limits_.max_loss) return reject(RiskCheckResult::LossTooLarge);
    if (context.traded_volume + order.qty.hundredths > limits_.max_daily_volume.hundredths) {
        return reject(RiskCheckResult::VolumeCapHit);
    }
    return RiskCheckResult::Allowed;
}

int64_t RiskManager::total_rejects() const {
    int64_t total = 0;
    for (int i = 1; i < 12; ++i) total += reject_counts_[i];
    return total;
}

}  // namespace mt
