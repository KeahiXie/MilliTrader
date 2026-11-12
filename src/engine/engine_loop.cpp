#include "engine/engine_loop.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "core/clock.hpp"

namespace mt {

EngineLoop::EngineLoop(const EngineConfig& config, const InstrumentTable& instruments, Queues queues,
                       JournalWriter* journal, HopQueue* hops, StatusQueue* status, const char* gateway_name, bool spin)
    : queues_(queues), journal_(journal), hops_(hops), status_(status), spin_(spin), engine_(config, instruments, *this) {
    copy_cstring(gateway_name_, sizeof(gateway_name_), gateway_name);
    if (journal_) journal_->push(config);  // replay starts from the same settings
}

void EngineLoop::sample(int hop, int64_t nanos) {
    if (hops_ != nullptr) hops_->try_push(HopSample{static_cast<uint8_t>(hop), nanos});  // drop if full: stats only
}

void EngineLoop::send_order(const OrderRequest& request) {
    OrderRequest stamped = request;
    stamped.decided_mono_ns = monotonic_ns();
    if (request.type == OrderRequestType::New) sample(kHopEngine, stamped.decided_mono_ns - current_dequeue_mono_ns_);
    if (journal_) journal_->push(OrderOutRecord{stamped, engine_.now_ns()});
    while (!queues_.orders_out->try_push(stamped)) cpu_relax();
}

void EngineLoop::on_risk_reject(const OrderRequest& request, RiskCheckResult reason) {
    if (journal_) journal_->push(RiskRejectRecord{request, reason, engine_.now_ns()});
}

void EngineLoop::run() {
    int64_t timers_since_shutdown = -1;
    int64_t timer_count = 0;
    FeedEvent feed_event;
    GatewayEvent gateway_event;
    ControlEvent control_event;

    while (true) {
        bool any = false;

        while (queues_.control->try_pop(control_event)) {
            any = true;
            current_dequeue_mono_ns_ = monotonic_ns();
            if (journal_) journal_->push(control_event);
            engine_.on_control_event(control_event);
            if (control_event.type == ControlEventType::Timer) {
                ++timer_count;
                if (timers_since_shutdown >= 0) ++timers_since_shutdown;
                // Status for the dashboard every 3rd tick (~3.3 Hz).
                if (status_ != nullptr && timer_count % 3 == 0) {
                    StatusSnapshot status;
                    engine_.fill_status(&status);
                    copy_cstring(status.gateway, sizeof(status.gateway), gateway_name_);
                    status_->try_push(status);
                }
            }
            if (engine_.shutdown_requested() && timers_since_shutdown < 0) timers_since_shutdown = 0;
        }

        while (queues_.gateway_events->try_pop(gateway_event)) {
            any = true;
            current_dequeue_mono_ns_ = monotonic_ns();
            if (journal_) journal_->push(gateway_event);
            engine_.on_gateway_event(gateway_event);
            sample(kHopEngineEvent, monotonic_ns() - current_dequeue_mono_ns_);
        }

        for (int i = 0; i < 256 && queues_.feed->try_pop(feed_event); ++i) {
            any = true;
            current_dequeue_mono_ns_ = monotonic_ns();
            if (feed_event.receive_mono_ns != 0) sample(kHopFeedDecode, feed_event.enqueue_mono_ns - feed_event.receive_mono_ns);
            sample(kHopFeedQueue, current_dequeue_mono_ns_ - feed_event.enqueue_mono_ns);
            if (journal_) journal_->push(feed_event);
            engine_.on_feed_event(feed_event);
            sample(kHopEngineEvent, monotonic_ns() - current_dequeue_mono_ns_);
        }

        // after Shutdown, keep running 30 timer ticks (3 s) so the cancels can be acked
        if (timers_since_shutdown >= 30) break;
        if (!any) {
            if (spin_) {
                cpu_relax();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }
}

}  // namespace mt
