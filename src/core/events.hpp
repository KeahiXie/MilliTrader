#pragma once
// Messages passed between threads. All fixed-size and trivially copyable so they copy into
// SPSC ring slots: no heap, no pointers, no strings (those stay in the feed/gateway threads).

#include <array>
#include <cstdint>
#include <type_traits>
#include <variant>

#include "core/types.hpp"

namespace mt {

// Market data: feed thread -> engine
enum class FeedEventType : uint8_t {
    BookClear,     // start of a snapshot: forget every level of this instrument
    BookLevel,     // snapshot level: set qty at price (absolute)
    BookDelta,     // incremental update: add qty (may be negative) at price
    BookSnapshotDone,  // snapshot finished, the book is consistent again
    BookStale,     // gap detected: don't trust or trade this book until BookSnapshotDone
    Trade,         // a trade printed on the venue
    MarketOpen,    // instrument started trading
    MarketClosed,  // instrument stopped trading (expiry, pause)
};

struct FeedEvent {
    FeedEventType type = FeedEventType::BookDelta;
    Venue venue = Venue::Kalshi;
    Side side = Side::Buy;          // Buy = bid side, Sell = ask side (YES contract)
    InstrumentId instrument = kNoInstrument;
    Price price{};
    Qty qty{};                      // level size, delta, or trade size depending on type
    uint64_t sequence = 0;          // venue sequence number (0 if the venue has none)
    int64_t receive_ns = 0;         // wall clock when the frame came off the socket
    int64_t exchange_ns = 0;        // venue timestamp, if it sends one
    int64_t receive_mono_ns = 0;    // monotonic receive time (stats only)
    int64_t enqueue_mono_ns = 0;    // monotonic enqueue time (stats only)
};

// Orders: engine -> gateway thread
enum class OrderRequestType : uint8_t { New, Cancel, Replace, Reconcile };

struct OrderRequest {
    OrderRequestType type = OrderRequestType::New;
    Side side = Side::Buy;          // Buy YES / Sell YES (gateways map Sell to "buy NO" if needed)
    bool post_only = true;
    InstrumentId instrument = kNoInstrument;
    OrderId order_id = 0;           // our id; client_order_id is derived from it
    Price price{};
    Qty qty{};
    int64_t decided_mono_ns = 0;    // when the engine decided (stats only)
};

// Order events: gateway thread -> engine
enum class GatewayEventType : uint8_t {
    Accepted,        // exchange acknowledged a new order (now resting)
    Rejected,        // exchange refused a new order
    Fill,            // part or all of an order traded
    Canceled,        // order is gone (cancel ack, or exchange-side cancel)
    CancelRejected,  // cancel refused (usually: already filled)
    Replaced,        // price/qty change acknowledged
    StatusReport,    // reconciliation answer: what the exchange thinks this order is
};

// Exchange's view of an order, sent in StatusReport during reconciliation.
enum class ExchangeOrderStatus : uint8_t { NotFound, Resting, Filled, Canceled };

struct GatewayEvent {
    GatewayEventType type = GatewayEventType::Accepted;
    Side side = Side::Buy;
    ExchangeOrderStatus status = ExchangeOrderStatus::NotFound;  // StatusReport only
    bool is_taker = false;           // Fill only: did our order take liquidity?
    InstrumentId instrument = kNoInstrument;
    OrderId order_id = 0;
    Price price{};                   // fill price (Fill) or new price (Replaced)
    Qty qty{};                       // fill qty (Fill) or new total qty (Replaced)
    Qty cumulative_filled{};         // exchange's running total, when it sends one
    uint64_t fill_id = 0;            // hash of the venue's fill/trade id, for duplicate detection
    MicroDollars fee = 0;            // fee charged on this fill, if the venue reports it
    int64_t receive_ns = 0;
    int64_t enqueue_mono_ns = 0;
    std::array<char, 40> reason{};   // reject reason, NUL-terminated, truncated
};

// Control: control thread -> engine
enum class ControlEventType : uint8_t {
    Timer,             // periodic tick; carries "now" so the engine never reads the clock
    KillSwitchOn,      // cancel everything, send nothing new
    KillSwitchOff,
    SetMaxOrderQty,    // value = hundredths of a contract
    SetMaxPosition,    // value = hundredths of a contract
    Shutdown,
};

struct ControlEvent {
    ControlEventType type = ControlEventType::Timer;
    int64_t value = 0;
    int64_t now_ns = 0;
    int64_t enqueue_mono_ns = 0;
};

// One engine input as journaled; replaying them in order reproduces every decision.
using EngineInput = std::variant<FeedEvent, GatewayEvent, ControlEvent>;

static_assert(std::is_trivially_copyable_v<FeedEvent>);
static_assert(std::is_trivially_copyable_v<OrderRequest>);
static_assert(std::is_trivially_copyable_v<GatewayEvent>);
static_assert(std::is_trivially_copyable_v<ControlEvent>);
static_assert(sizeof(FeedEvent) <= 64, "FeedEvent should fit in one cache line");

}  // namespace mt
