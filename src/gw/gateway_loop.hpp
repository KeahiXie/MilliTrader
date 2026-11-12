#pragma once
// GatewayLoop: the gateway thread. Passes engine OrderRequests to the OrderGateway, polls it, and
// pushes its GatewayEvents back to the engine as that queue's single producer.

#include <atomic>

#include "core/spsc_queue.hpp"
#include "gw/order_gateway.hpp"
#include "io/stats.hpp"

namespace mt {

class GatewayLoop : public GatewayEventSink {
public:
    GatewayLoop(OrderGateway& gateway, SpscQueue<OrderRequest>& requests, SpscQueue<GatewayEvent>& events,
                HopQueue* hops);

    void run(const std::atomic<bool>& running);
    void on_gateway_event(const GatewayEvent& event) override;

    int64_t requests_sent() const { return requests_sent_; }

private:
    OrderGateway& gateway_;
    SpscQueue<OrderRequest>& requests_;
    SpscQueue<GatewayEvent>& events_;
    HopQueue* hops_;
    int64_t requests_sent_ = 0;
};

}  // namespace mt
