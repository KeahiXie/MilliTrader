#pragma once
// StatusSnapshot: fixed-size struct sent to the dashboard as one Unix datagram, 4x a second.
// If the dashboard is slow or not running, the datagram is dropped, so it can't slow the engine.

#include <cstdint>

#include "core/types.hpp"

namespace mt {

inline constexpr uint32_t kStatusMagic = 0x4D545354;  // "MTST"
inline constexpr uint32_t kStatusVersion = 1;
inline const char* kStatusSocketPath = "/tmp/millitrader-status.sock";
inline const char* kControlSocketPath = "/tmp/millitrader-control.sock";

struct InstrumentStatus {
    char symbol[48] = {};
    bool tradable = false;
    bool has_bid = false;
    bool has_ask = false;
    Price bid{}, ask{};
    Qty bid_qty{}, ask_qty{};
    int64_t position = 0;          // hundredths of a YES contract
    MicroDollars realized = 0;
    MicroDollars unrealized = 0;
    MicroDollars fees = 0;
};

struct OpenOrderStatus {
    uint64_t order_id = 0;
    InstrumentId instrument = kNoInstrument;
    Side side = Side::Buy;
    uint8_t state = 0;             // OrderState
    Price price{};
    Qty qty{};
    Qty filled{};
};

enum Hop : int {
    kHopFeedDecode = 0,   // socket read -> event queued (feed thread: parse + book build)
    kHopFeedQueue,        // queued -> engine picked it up
    kHopEngine,           // engine picked it up -> order decided (tick-to-order)
    kHopGatewayQueue,     // order decided -> gateway thread picked it up
    kHopGatewaySend,      // time inside the gateway's send (the network round trip for REST)
    kHopEngineEvent,      // engine time for any event, order or not
    kHopCount,
};

inline const char* kHopNames[kHopCount] = {"feed decode", "feed queue", "engine->order", "gateway queue",
                                          "gateway send", "engine event"};

struct HopStatus {
    int64_t count = 0;
    int64_t p50_ns = 0;
    int64_t p99_ns = 0;
    int64_t p999_ns = 0;
};

struct StatusSnapshot {
    uint32_t magic = kStatusMagic;
    uint32_t version = kStatusVersion;
    int64_t engine_time_ns = 0;
    char gateway[16] = {};
    bool kill_switch = false;

    uint32_t instrument_count = 0;
    InstrumentStatus instruments[4];
    uint32_t open_order_count = 0;
    OpenOrderStatus open_orders[8];

    int64_t feed_events = 0;
    int64_t orders_sent = 0;
    int64_t fills = 0;
    int64_t risk_rejects = 0;
    int64_t unknown_orders = 0;
    MicroDollars total_pnl = 0;

    HopStatus hops[kHopCount];
    int64_t feed_gaps = 0;
    int64_t feed_recoveries = 0;
    int64_t last_feed_age_ms = 0;
};

}  // namespace mt
