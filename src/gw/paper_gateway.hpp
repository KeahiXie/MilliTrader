#pragma once
// PaperGateway: simulated venue for exercising the order path without a network. Answers after
// `latency_ns`, fills resting orders at a seeded random rate, rejects a fraction, and can go silent
// to exercise timeout -> Unknown -> reconcile (reconcile is always answered).

#include <cstdint>
#include <deque>
#include <map>
#include <random>

#include "gw/order_gateway.hpp"

namespace mt {

struct PaperGatewayConfig {
    int64_t latency_ns = 2'000'000;            // 2 ms
    double fill_rate_per_second = 0.05;        // a resting order fills after ~20 s on average
    double reject_probability = 0.0;
    int64_t silent_after_orders = 0;           // 0 = never go silent
    int64_t silent_for_ns = 10'000'000'000;    // 10 s
    uint32_t seed = 42;
};

class PaperGateway : public OrderGateway {
public:
    explicit PaperGateway(PaperGatewayConfig config);

    const char* name() const override { return "paper"; }
    bool start(GatewayEventSink& sink) override;
    void send(const OrderRequest& request) override;
    void poll(int64_t now_ns) override;
    void reconcile(const std::vector<OrderId>& order_ids) override;
    void cancel_all() override;
    void stop() override {}

private:
    struct RestingOrder {
        OrderRequest order;
        Qty filled{};
    };
    struct Pending {
        int64_t due_ns;
        GatewayEvent event;
    };

    void answer(GatewayEvent event, int64_t now_ns);
    bool silent(int64_t now_ns) const;

    PaperGatewayConfig config_;
    GatewayEventSink* sink_ = nullptr;
    std::mt19937_64 rng_;
    std::map<OrderId, RestingOrder> resting_;
    struct Finished {
        ExchangeOrderStatus status = ExchangeOrderStatus::NotFound;
        Qty filled{};
        Price price{};
    };
    std::map<OrderId, Finished> finished_;  // for reconcile answers
    std::deque<Pending> pending_;
    int64_t orders_seen_ = 0;
    int64_t silent_until_ns_ = 0;
    int64_t last_poll_ns_ = 0;
    uint64_t next_fill_id_ = 1;
};

}  // namespace mt
