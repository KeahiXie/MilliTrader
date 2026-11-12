#include "gw/paper_gateway.hpp"

#include <cmath>
#include <cstring>

#include "core/clock.hpp"

namespace mt {

PaperGateway::PaperGateway(PaperGatewayConfig config) : config_(config), rng_(config.seed) {}

bool PaperGateway::start(GatewayEventSink& sink) {
    sink_ = &sink;
    return true;
}

bool PaperGateway::silent(int64_t now_ns) const { return now_ns < silent_until_ns_; }

void PaperGateway::answer(GatewayEvent event, int64_t now_ns) {
    pending_.push_back({now_ns + config_.latency_ns, event});
}

void PaperGateway::send(const OrderRequest& request) {
    const int64_t now = wall_clock_ns();
    GatewayEvent event;
    event.order_id = request.order_id;
    event.instrument = request.instrument;
    event.side = request.side;

    switch (request.type) {
        case OrderRequestType::New: {
            ++orders_seen_;
            if (config_.silent_after_orders > 0 && orders_seen_ == config_.silent_after_orders) {
                silent_until_ns_ = now + config_.silent_for_ns;  // the "venue" goes quiet from here
            }
            std::uniform_real_distribution<double> uniform(0.0, 1.0);
            if (uniform(rng_) < config_.reject_probability) {
                event.type = GatewayEventType::Rejected;
                std::strncpy(event.reason.data(), "paper: random reject", event.reason.size() - 1);
                finished_[request.order_id] = Finished{ExchangeOrderStatus::NotFound, Qty{}, request.price};
            } else {
                event.type = GatewayEventType::Accepted;
                resting_[request.order_id] = RestingOrder{request, Qty{}};
            }
            if (!silent(now)) answer(event, now);
            break;
        }
        case OrderRequestType::Cancel: {
            auto it = resting_.find(request.order_id);
            if (it == resting_.end()) {
                event.type = GatewayEventType::CancelRejected;
                std::strncpy(event.reason.data(), "paper: not resting", event.reason.size() - 1);
            } else {
                event.type = GatewayEventType::Canceled;
                finished_[request.order_id] = Finished{ExchangeOrderStatus::Canceled, it->second.filled, request.price};
                resting_.erase(it);
            }
            if (!silent(now)) answer(event, now);
            break;
        }
        case OrderRequestType::Replace: {
            auto it = resting_.find(request.order_id);
            if (it == resting_.end()) {
                event.type = GatewayEventType::CancelRejected;
            } else {
                it->second.order.price = request.price;
                it->second.order.qty = request.qty;
                event.type = GatewayEventType::Replaced;
                event.price = request.price;
                event.qty = request.qty;
            }
            if (!silent(now)) answer(event, now);
            break;
        }
        case OrderRequestType::Reconcile:
            reconcile({request.order_id});
            break;
    }
}

void PaperGateway::reconcile(const std::vector<OrderId>& order_ids) {
    const int64_t now = wall_clock_ns();
    for (const OrderId id : order_ids) {
        GatewayEvent report;
        report.type = GatewayEventType::StatusReport;
        report.order_id = id;
        if (auto it = resting_.find(id); it != resting_.end()) {
            report.status = ExchangeOrderStatus::Resting;
            report.cumulative_filled = it->second.filled;
            report.instrument = it->second.order.instrument;
        } else if (auto done = finished_.find(id); done != finished_.end()) {
            report.status = done->second.status;
            report.cumulative_filled = done->second.filled;
            report.price = done->second.price;
        } else {
            report.status = ExchangeOrderStatus::NotFound;
        }
        // Reconcile is answered even while silent.
        pending_.push_back({now + config_.latency_ns, report});
    }
}

void PaperGateway::cancel_all() {
    for (const auto& [id, order] : resting_) {
        OrderRequest cancel = order.order;
        cancel.type = OrderRequestType::Cancel;
        send(cancel);
    }
}

void PaperGateway::poll(int64_t now_ns) {
    // Each resting order fills with probability 1 - exp(-rate * elapsed).
    if (last_poll_ns_ != 0 && !resting_.empty()) {
        const double elapsed_s = static_cast<double>(now_ns - last_poll_ns_) / 1e9;
        const double probability = 1.0 - std::exp(-config_.fill_rate_per_second * elapsed_s);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        for (auto it = resting_.begin(); it != resting_.end();) {
            if (uniform(rng_) >= probability) {
                ++it;
                continue;
            }
            GatewayEvent fill;
            fill.type = GatewayEventType::Fill;
            fill.order_id = it->first;
            fill.instrument = it->second.order.instrument;
            fill.side = it->second.order.side;
            fill.price = it->second.order.price;
            fill.qty = Qty{it->second.order.qty.hundredths - it->second.filled.hundredths};
            fill.cumulative_filled = it->second.order.qty;
            fill.fill_id = next_fill_id_++;
            fill.is_taker = false;  // a resting quote that got hit is a maker fill
            finished_[it->first] = Finished{ExchangeOrderStatus::Filled, it->second.order.qty, it->second.order.price};
            it = resting_.erase(it);
            if (!silent(now_ns)) answer(fill, now_ns);
        }
    }
    last_poll_ns_ = now_ns;

    // Deliver due events in order.
    while (!pending_.empty() && pending_.front().due_ns <= now_ns) {
        GatewayEvent event = pending_.front().event;
        pending_.pop_front();
        event.receive_ns = now_ns;
        sink_->on_gateway_event(event);
    }
}

}  // namespace mt
