#pragma once
// EngineLoop: the pinned engine thread's run loop. Each pass: control first (a kill switch
// must not wait behind book updates), then gateway events, then up to 256 feed events.
// Every input is journaled before it is handled; that order is what replay reproduces.

#include <atomic>
#include <cstdint>

#include "core/spsc_queue.hpp"
#include "engine/journal.hpp"
#include "engine/trade_engine.hpp"
#include "io/control_channel.hpp"
#include "io/stats.hpp"
#include "md/market_data_consumer.hpp"

namespace mt {

using OrderQueue = SpscQueue<OrderRequest>;
using GatewayEventQueue = SpscQueue<GatewayEvent>;

class EngineLoop : public OrderSink {
public:
    struct Queues {
        FeedQueue* feed = nullptr;
        GatewayEventQueue* gateway_events = nullptr;
        ControlQueue* control = nullptr;
        OrderQueue* orders_out = nullptr;
    };

    EngineLoop(const EngineConfig& config, const InstrumentTable& instruments, Queues queues, JournalWriter* journal,
               HopQueue* hops, StatusQueue* status, const char* gateway_name, bool spin);

    void run();  // returns after Shutdown, once cancels have had time to settle
    TradeEngine& engine() { return engine_; }

    // OrderSink (called by the engine, on this thread)
    void send_order(const OrderRequest& request) override;
    void on_risk_reject(const OrderRequest& request, RiskCheckResult reason) override;

private:
    void sample(int hop, int64_t nanos);

    Queues queues_;
    JournalWriter* journal_;
    HopQueue* hops_;
    StatusQueue* status_;
    char gateway_name_[16] = {};
    bool spin_;  // busy-poll when idle; otherwise sleep 50 us per idle pass (--no-spin)
    TradeEngine engine_;
    int64_t current_dequeue_mono_ns_ = 0;  // when the event being handled was popped
};

}  // namespace mt
