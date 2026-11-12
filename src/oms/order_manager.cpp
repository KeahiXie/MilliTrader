#include "oms/order_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace mt {

const char* to_string(OrderState state) {
    switch (state) {
        case OrderState::PendingNew: return "PendingNew";
        case OrderState::New: return "New";
        case OrderState::PartiallyFilled: return "PartiallyFilled";
        case OrderState::Filled: return "Filled";
        case OrderState::PendingCancel: return "PendingCancel";
        case OrderState::Canceled: return "Canceled";
        case OrderState::PendingReplace: return "PendingReplace";
        case OrderState::Rejected: return "Rejected";
        case OrderState::Unknown: return "Unknown";
    }
    return "?";
}

bool is_final(OrderState state) {
    return state == OrderState::Filled || state == OrderState::Canceled || state == OrderState::Rejected;
}

namespace {

bool is_pending(OrderState state) {
    return state == OrderState::PendingNew || state == OrderState::PendingCancel ||
           state == OrderState::PendingReplace;
}

// The resting state for an order with `filled` out of `qty`.
OrderState working_state(const Order& order) {
    if (order.filled >= order.qty) return OrderState::Filled;
    return order.filled.hundredths > 0 ? OrderState::PartiallyFilled : OrderState::New;
}

}  // namespace

OrderManager::OrderManager(int64_t request_timeout_ns, size_t expected_orders)
    : timeout_ns_(request_timeout_ns), seen_fill_ids_(1 << 17) {
    orders_.reserve(expected_orders);
}

Order* OrderManager::find_mutable(OrderId id) {
    if (id == 0 || id > orders_.size()) return nullptr;
    return &orders_[id - 1];
}

const Order* OrderManager::find(OrderId id) const {
    if (id == 0 || id > orders_.size()) return nullptr;
    return &orders_[id - 1];
}

const Order& OrderManager::create(OrderId id, InstrumentId instrument, Side side, Price price, Qty qty,
                                  int64_t now_ns) {
    // ids are sequential from 1, so the slot is orders_[id - 1]
    if (id != orders_.size() + 1) throw std::logic_error("order ids must be sequential");
    Order order;
    order.id = id;
    order.instrument = instrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.state = OrderState::PendingNew;
    order.created_ns = now_ns;
    order.pending_since_ns = now_ns;
    orders_.push_back(order);
    return orders_.back();
}

bool OrderManager::request_cancel(OrderId id, int64_t now_ns) {
    Order* order = find_mutable(id);
    if (order == nullptr) return false;
    // Only resting orders: a cancel before the ack just gets rejected; Unknown waits for reconcile.
    if (order->state != OrderState::New && order->state != OrderState::PartiallyFilled) return false;
    if (order->last_cancel_reject_ns != 0 && now_ns - order->last_cancel_reject_ns < kCancelRetryBackoff) return false;
    order->state_before_pending = order->state;
    order->state = OrderState::PendingCancel;
    order->pending_since_ns = now_ns;
    return true;
}

bool OrderManager::request_replace(OrderId id, Price new_price, Qty new_qty, int64_t now_ns) {
    Order* order = find_mutable(id);
    if (order == nullptr) return false;
    if (order->state != OrderState::New && order->state != OrderState::PartiallyFilled) return false;
    order->state_before_pending = order->state;
    order->state = OrderState::PendingReplace;
    order->replace_price = new_price;
    order->replace_qty = new_qty;
    order->pending_since_ns = now_ns;
    return true;
}

OrderUpdate OrderManager::apply_fill(Order& order, const GatewayEvent& event) {
    OrderUpdate update;
    update.known = true;
    if (event.fill_id != 0 && !seen_fill_ids_.insert(event.fill_id)) {
        ++duplicate_fills_;
        update.duplicate = true;
        return update;
    }
    // Never book more than leaves(): an overfill shows up in the counters, not as a phantom position.
    const Qty fill = std::min(event.qty, order.leaves());
    if (fill.hundredths <= 0) {
        ++ignored_events_;
        update.ignored = true;
        return update;
    }
    order.filled += fill;
    update.fill_qty = fill;
    update.fill_price = event.price;
    update.is_taker = event.is_taker;
    update.fee = event.fee;
    return update;
}

void OrderManager::settle_unknown(Order& order, const GatewayEvent& report, OrderUpdate* update) {
    // The venue's cumulative fill is the truth: book any fills we missed.
    if (report.cumulative_filled > order.filled) {
        const Qty missed = std::min(Qty{report.cumulative_filled.hundredths - order.filled.hundredths}, order.leaves());
        order.filled += missed;
        update->fill_qty = missed;
        update->fill_price = report.price.ticks > 0 ? report.price : order.price;
    }
    switch (report.status) {
        case ExchangeOrderStatus::Resting: order.state = working_state(order); break;
        case ExchangeOrderStatus::Filled: order.state = OrderState::Filled; break;
        case ExchangeOrderStatus::Canceled: order.state = OrderState::Canceled; break;
        case ExchangeOrderStatus::NotFound:
            // Never reached the venue: it was a new order that got lost.
            order.state = order.state_before_unknown == OrderState::PendingNew ? OrderState::Rejected
                                                                              : OrderState::Canceled;
            break;
    }
}

OrderUpdate OrderManager::on_gateway_event(const GatewayEvent& event, int64_t now_ns) {
    Order* order = find_mutable(event.order_id);
    if (order == nullptr) return OrderUpdate{};

    OrderUpdate update;
    update.known = true;
    update.old_state = order->state;
    const OrderState before_unknown = order->state_before_unknown;
    const bool unknown = order->state == OrderState::Unknown;
    auto ignore = [&] {
        ++ignored_events_;
        update.ignored = true;
    };

    switch (event.type) {
        case GatewayEventType::Accepted:
            if (order->state == OrderState::PendingNew || (unknown && before_unknown == OrderState::PendingNew)) {
                order->state = working_state(*order);
            } else {
                ignore();  // e.g. the ack arrived after a fill already implied it
            }
            break;

        case GatewayEventType::Rejected:
            if (order->state == OrderState::PendingNew || (unknown && before_unknown == OrderState::PendingNew)) {
                order->state = OrderState::Rejected;
            } else if (order->state == OrderState::PendingReplace) {
                order->state = working_state(*order);  // replace refused: the old order still rests
            } else {
                ignore();
            }
            break;

        case GatewayEventType::Fill: {
            const OrderState old_state = order->state;
            update = apply_fill(*order, event);
            update.old_state = old_state;
            if (update.duplicate || update.ignored) break;
            if (order->filled >= order->qty) {
                order->state = OrderState::Filled;
            } else if (order->state == OrderState::PendingNew || order->state == OrderState::New) {
                order->state = OrderState::PartiallyFilled;  // a fill before the ack implies the ack
            } else if (unknown) {
                // fill proves it's alive; if we were canceling, we still are
                order->state = before_unknown == OrderState::PendingCancel ? OrderState::PendingCancel
                                                                           : OrderState::PartiallyFilled;
                order->pending_since_ns = now_ns;
            }
            // otherwise any pending request is still outstanding; the fill only reduced leaves
            break;
        }

        case GatewayEventType::Canceled:
            if (is_final(order->state)) {
                ignore();
            } else {
                order->state = OrderState::Canceled;  // solicited or not (expiry, post-only cross, kill switch)
            }
            break;

        case GatewayEventType::CancelRejected:
            if (order->state == OrderState::PendingCancel) order->last_cancel_reject_ns = now_ns;
            if (order->state == OrderState::PendingCancel || order->state == OrderState::PendingReplace || unknown) {
                order->state = working_state(*order);  // usually "too late, it filled"; fills settle the rest
            } else {
                ignore();
            }
            break;

        case GatewayEventType::Replaced:
            if (order->state == OrderState::PendingReplace) {
                order->price = event.price.ticks > 0 ? event.price : order->replace_price;
                order->qty = event.qty.hundredths > 0 ? event.qty : order->replace_qty;
                order->state = working_state(*order);
            } else {
                ignore();
            }
            break;

        case GatewayEventType::StatusReport:
            if (unknown) {
                settle_unknown(*order, event, &update);
            } else {
                ignore();  // we already know; reconcile answers can arrive late
            }
            break;
    }
    update.new_state = order->state;
    return update;
}

void OrderManager::on_timer(int64_t now_ns, std::vector<OrderId>* became_unknown) {
    for (Order& order : orders_) {
        const bool waiting = is_pending(order.state) || order.state == OrderState::Unknown;
        if (!waiting || now_ns - order.pending_since_ns < timeout_ns_) continue;
        if (order.state != OrderState::Unknown) order.state_before_unknown = order.state;
        order.state = OrderState::Unknown;
        order.pending_since_ns = now_ns;  // ask again after another timeout if still unanswered
        if (became_unknown) became_unknown->push_back(order.id);
    }
}

Qty OrderManager::open_qty(InstrumentId instrument, Side side) const {
    Qty total{};
    for (const Order& order : orders_) {
        if (order.instrument == instrument && order.side == side && !is_final(order.state)) total += order.leaves();
    }
    return total;
}

int OrderManager::open_order_count() const {
    return static_cast<int>(std::count_if(orders_.begin(), orders_.end(), [](const Order& o) { return !is_final(o.state); }));
}

int OrderManager::unknown_count() const {
    return static_cast<int>(
        std::count_if(orders_.begin(), orders_.end(), [](const Order& o) { return o.state == OrderState::Unknown; }));
}

uint64_t OrderManager::state_hash() const {
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (const Order& order : orders_) {
        mix(order.id);
        mix(static_cast<uint64_t>(order.state));
        mix(static_cast<uint64_t>(order.filled.hundredths));
        mix(static_cast<uint64_t>(order.price.ticks));
        mix(static_cast<uint64_t>(order.qty.hundredths));
    }
    return hash;
}

}  // namespace mt
