#pragma once
// OrderManager: state machine for every order this session sends.
// A request with no answer before the timeout becomes Unknown and is settled by reconcile
// (asking the venue by client order id), never by resending: a resend could double the position.

#include <cstdint>
#include <vector>

#include "core/events.hpp"
#include "oms/fixed_hash_set.hpp"

namespace mt {

enum class OrderState : uint8_t {
    PendingNew,
    New,
    PartiallyFilled,
    Filled,
    PendingCancel,
    Canceled,
    PendingReplace,
    Rejected,
    Unknown,
};

const char* to_string(OrderState state);
bool is_final(OrderState state);  // Filled, Canceled, Rejected

struct Order {
    OrderId id = 0;
    InstrumentId instrument = kNoInstrument;
    Side side = Side::Buy;
    Price price{};
    Qty qty{};
    Qty filled{};
    OrderState state = OrderState::PendingNew;
    OrderState state_before_unknown = OrderState::PendingNew;
    OrderState state_before_pending = OrderState::New;  // where a rejected cancel/replace returns to
    Price replace_price{};
    Qty replace_qty{};
    int64_t created_ns = 0;
    int64_t pending_since_ns = 0;  // when the outstanding request was sent
    int64_t last_cancel_reject_ns = 0;

    Qty leaves() const { return Qty{qty.hundredths - filled.hundredths}; }
};

// What one gateway event did, so the engine can update positions.
struct OrderUpdate {
    bool known = false;          // the event matched one of our orders
    bool duplicate = false;      // fill id already seen: ignored
    bool ignored = false;        // event made no sense in the current state (counted, not applied)
    Qty fill_qty{};              // > 0 if a fill must be booked
    Price fill_price{};
    bool is_taker = false;
    MicroDollars fee = 0;
    OrderState old_state = OrderState::PendingNew;
    OrderState new_state = OrderState::PendingNew;
};

// Wait this long before re-canceling after a CancelRejected, or cancel/reject loops at queue speed.
inline constexpr int64_t kCancelRetryBackoff = 1'000'000'000;  // 1 s

class OrderManager {
public:
    explicit OrderManager(int64_t request_timeout_ns, size_t expected_orders = 1 << 16);

    // Called before the request is sent, so a response never arrives for an order we don't know.
    const Order& create(OrderId id, InstrumentId instrument, Side side, Price price, Qty qty, int64_t now_ns);
    bool request_cancel(OrderId id, int64_t now_ns);
    bool request_replace(OrderId id, Price new_price, Qty new_qty, int64_t now_ns);

    OrderUpdate on_gateway_event(const GatewayEvent& event, int64_t now_ns);

    // Requests past the timeout become Unknown; their ids go to became_unknown for reconcile.
    // Driven by timer events, not the clock, so a replay produces the same Unknowns.
    void on_timer(int64_t now_ns, std::vector<OrderId>* became_unknown);

    const Order* find(OrderId id) const;
    const std::vector<Order>& all_orders() const { return orders_; }

    // Over working (non-final) orders only.
    Qty open_qty(InstrumentId instrument, Side side) const;
    int open_order_count() const;
    int unknown_count() const;
    int64_t ignored_events() const { return ignored_events_; }
    int64_t duplicate_fills() const { return duplicate_fills_; }

    uint64_t state_hash() const;

private:
    Order* find_mutable(OrderId id);
    OrderUpdate apply_fill(Order& order, const GatewayEvent& event);
    void settle_unknown(Order& order, const GatewayEvent& report, OrderUpdate* update);

    int64_t timeout_ns_;
    std::vector<Order> orders_;     // orders_[id - 1]: ids are sequential, O(1) lookup
    FixedHashSet seen_fill_ids_;    // a repeated fill id is ignored, never booked twice
    int64_t ignored_events_ = 0;
    int64_t duplicate_fills_ = 0;
};

}  // namespace mt
