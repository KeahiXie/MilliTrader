#pragma once
// KalshiRestGateway: orders on Kalshi's DEMO exchange over signed REST, with order and fill events
// from the private WebSocket channels (user_orders, fill). WebSocket callbacks fill a locked inbox
// that poll() drains on the gateway thread.

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/instruments.hpp"
#include "gw/order_gateway.hpp"
#include "io/auth.hpp"
#include "io/websocket_client.hpp"
#include "md/kalshi_rest.hpp"

namespace mt {

// "0.010000" dollars -> 10'000 micro-dollars (Kalshi reports fees with 6 decimals).
MicroDollars parse_scaled_micro(const std::string& text);

struct KalshiGatewayConfig {
    std::string rest_url = kKalshiDemoRestUrl;
    std::string ws_url = kKalshiDemoWsUrl;
    uint32_t session_id = 1;
    int64_t order_group_contracts_limit = 10;  // exchange-side cap on filled contracts per 15 s
    bool cancel_leftovers_on_start = true;
};

class KalshiRestGateway : public OrderGateway {
public:
    KalshiRestGateway(KalshiGatewayConfig config, const InstrumentTable& instruments, KalshiSigner signer);
    ~KalshiRestGateway() override;

    const char* name() const override { return "kalshi-demo"; }
    bool start(GatewayEventSink& sink) override;
    void send(const OrderRequest& request) override;
    void poll(int64_t now_ns) override;
    void reconcile(const std::vector<OrderId>& order_ids) override;
    void cancel_all() override;
    void stop() override;

    const std::string& order_group_id() const { return order_group_id_; }

private:
    void place(const OrderRequest& request);
    void cancel(const OrderRequest& request);
    void amend(const OrderRequest& request);
    void on_ws_message(const std::string& text);
    void deliver(GatewayEvent event);
    GatewayEvent base_event(GatewayEventType type, OrderId id) const;
    std::string ticker_of(InstrumentId id) const;

    KalshiGatewayConfig config_;
    const InstrumentTable& instruments_;
    KalshiSigner signer_;
    KalshiRestClient rest_;
    WebSocketClient socket_;
    GatewayEventSink* sink_ = nullptr;
    std::string order_group_id_;

    // Our order id -> Kalshi's order id (gateway thread only).
    std::map<OrderId, std::string> exchange_ids_;
    std::map<OrderId, InstrumentId> instruments_by_order_;

    std::mutex inbox_mutex_;
    std::vector<GatewayEvent> inbox_;  // from the WebSocket thread
    int64_t started_ns_ = 0;
};

}  // namespace mt
