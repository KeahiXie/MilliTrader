#include "gw/gateway_loop.hpp"

#include <chrono>
#include <thread>

#include "core/clock.hpp"
#include "core/status.hpp"

namespace mt {

GatewayLoop::GatewayLoop(OrderGateway& gateway, SpscQueue<OrderRequest>& requests, SpscQueue<GatewayEvent>& events,
                         HopQueue* hops)
    : gateway_(gateway), requests_(requests), events_(events), hops_(hops) {}

void GatewayLoop::on_gateway_event(const GatewayEvent& event) {
    GatewayEvent stamped = event;
    if (stamped.receive_ns == 0) stamped.receive_ns = wall_clock_ns();
    stamped.enqueue_mono_ns = monotonic_ns();
    while (!events_.try_push(stamped)) cpu_relax();
}

void GatewayLoop::run(const std::atomic<bool>& running) {
    OrderRequest request;
    while (running) {
        bool any = false;
        while (requests_.try_pop(request)) {
            any = true;
            const int64_t picked_up = monotonic_ns();
            if (hops_ && request.decided_mono_ns != 0) {
                hops_->try_push(HopSample{kHopGatewayQueue, picked_up - request.decided_mono_ns});
            }
            if (request.type == OrderRequestType::Reconcile) {
                gateway_.reconcile({request.order_id});
            } else {
                gateway_.send(request);
                ++requests_sent_;
            }
            if (hops_ && request.type == OrderRequestType::New) {
                hops_->try_push(HopSample{kHopGatewaySend, monotonic_ns() - picked_up});
            }
        }
        gateway_.poll(wall_clock_ns());
        // Sleep, don't spin: the work is network-bound and this thread shares cores with the feeds.
        if (!any) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    gateway_.stop();
}

}  // namespace mt
