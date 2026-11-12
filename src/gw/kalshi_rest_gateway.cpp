#include "gw/kalshi_rest_gateway.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "core/clock.hpp"
#include "md/kalshi_ws_protocol.hpp"
#include "oms/fixed_hash_set.hpp"

namespace mt {

using nlohmann::json;

namespace {

void copy_reason(GatewayEvent& event, const std::string& text) {
    std::strncpy(event.reason.data(), text.c_str(), event.reason.size() - 1);
}

std::optional<Qty> qty_field(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return std::nullopt;
    return parse_qty(it->get<std::string>());
}

std::optional<Price> price_field(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return std::nullopt;
    return parse_price(it->get<std::string>());
}

}  // namespace

KalshiRestGateway::KalshiRestGateway(KalshiGatewayConfig config, const InstrumentTable& instruments,
                                     KalshiSigner signer)
    : config_(std::move(config)), instruments_(instruments), signer_(std::move(signer)), rest_(config_.rest_url, &signer_) {
    // v1 is demo-only, enforced here: no code path can send a real-money order.
    if (config_.rest_url.find(".demo.kalshi.co") == std::string::npos ||
        config_.ws_url.find(".demo.kalshi.co") == std::string::npos) {
        throw std::invalid_argument("KalshiRestGateway: orders are demo-only in v1 (refusing " + config_.rest_url + ")");
    }
}

KalshiRestGateway::~KalshiRestGateway() { socket_.close(); }

std::string KalshiRestGateway::ticker_of(InstrumentId id) const {
    const InstrumentInfo* info = instruments_.find(id);
    return info ? std::string(info->symbol_view()) : "";
}

GatewayEvent KalshiRestGateway::base_event(GatewayEventType type, OrderId id) const {
    GatewayEvent event;
    event.type = type;
    event.order_id = id;
    event.receive_ns = wall_clock_ns();
    if (auto it = instruments_by_order_.find(id); it != instruments_by_order_.end()) event.instrument = it->second;
    return event;
}

void KalshiRestGateway::deliver(GatewayEvent event) {
    if (event.receive_ns == 0) event.receive_ns = wall_clock_ns();
    sink_->on_gateway_event(event);
}

bool KalshiRestGateway::start(GatewayEventSink& sink) {
    sink_ = &sink;
    started_ns_ = wall_clock_ns();

    // Prove the key works before anything else.
    const HttpResponse balance = rest_.signed_get("/portfolio/balance");
    if (!balance.ok()) {
        std::fprintf(stderr, "kalshi-demo: balance check failed (http %d): %.200s\n", balance.status, balance.body.c_str());
        return false;
    }
    std::fprintf(stderr, "kalshi-demo: connected, balance %s\n", balance.body.c_str());

    if (config_.cancel_leftovers_on_start) {
        // Anything still resting is left over from an earlier session (crash, kill -9): cancel it.
        int64_t resting = -1;
        const HttpResponse open_orders = rest_.signed_get("/portfolio/orders?status=resting&limit=1000");
        if (open_orders.ok()) {
            const json parsed = json::parse(open_orders.body, nullptr, false);
            if (parsed.is_object() && parsed.contains("orders") && parsed["orders"].is_array()) {
                resting = static_cast<int64_t>(parsed["orders"].size());
            }
        }
        const HttpResponse cancelled = rest_.signed_delete("/portfolio/events/orders");
        std::fprintf(stderr, "kalshi-demo: found %lld resting orders from earlier sessions; cancel-all -> http %d\n",
                     static_cast<long long>(resting), cancelled.status);
    }

    // Order group: exchange-side cap on fills per 15 s; cancel_all() triggers it as a kill switch.
    const json group_body = {{"contracts_limit", config_.order_group_contracts_limit}};
    const HttpResponse group = rest_.signed_post("/portfolio/order_groups/create", group_body.dump());
    if (group.ok()) {
        const json parsed = json::parse(group.body, nullptr, false);
        if (parsed.is_object()) order_group_id_ = parsed.value("order_group_id", "");
        std::fprintf(stderr, "kalshi-demo: order group %s (limit %lld contracts / 15 s)\n", order_group_id_.c_str(),
                     static_cast<long long>(config_.order_group_contracts_limit));
    } else {
        std::fprintf(stderr, "kalshi-demo: order group not created (http %d), local kill switch only\n", group.status);
    }

    // Private channels: our own order updates and fills.
    WebSocketClient::Callbacks callbacks;
    callbacks.on_open = [this] {
        socket_.send_text(kalshi_subscribe_command(1, {"user_orders", "fill"}, {}));
    };
    callbacks.on_message = [this](int64_t, const std::string& text) { on_ws_message(text); };
    callbacks.on_close = [](const std::string& reason) {
        std::fprintf(stderr, "kalshi-demo: private websocket closed (%s); REST reconcile still works\n", reason.c_str());
    };
    socket_.connect(config_.ws_url, signer_.headers(wall_clock_ns() / kNanosPerMilli, "GET", kKalshiWsSignPath),
                    std::move(callbacks));
    return true;
}

void KalshiRestGateway::send(const OrderRequest& request) {
    switch (request.type) {
        case OrderRequestType::New: place(request); break;
        case OrderRequestType::Cancel: cancel(request); break;
        case OrderRequestType::Replace: amend(request); break;
        case OrderRequestType::Reconcile: reconcile({request.order_id}); break;
    }
}

void KalshiRestGateway::place(const OrderRequest& request) {
    instruments_by_order_[request.order_id] = request.instrument;
    json body = {
        {"ticker", ticker_of(request.instrument)},
        {"client_order_id", make_client_order_id(config_.session_id, request.order_id)},
        {"side", request.side == Side::Buy ? "bid" : "ask"},  // bid = buy YES, ask = sell YES (V2 API)
        {"count", to_string(request.qty)},
        {"price", to_string(request.price)},
        {"time_in_force", "good_till_canceled"},
        {"self_trade_prevention_type", "taker_at_cross"},
        {"post_only", request.post_only},
    };
    if (!order_group_id_.empty()) body["order_group_id"] = order_group_id_;

    const HttpResponse response = rest_.signed_post("/portfolio/events/orders", body.dump());
    if (response.status == 0) {
        // No response, so it may or may not have reached Kalshi. Never resend (duplicate client ids are not
        // guaranteed to be rejected); the engine's timeout marks it Unknown and reconciles.
        std::fprintf(stderr, "kalshi-demo: order %llu: no response (%s), will reconcile\n",
                     static_cast<unsigned long long>(request.order_id), response.error.c_str());
        return;
    }
    // Any HTTP error, 429 included, means not placed: report Rejected, never retry.
    if (!response.ok()) {
        GatewayEvent reject = base_event(GatewayEventType::Rejected, request.order_id);
        copy_reason(reject, "http " + std::to_string(response.status) + " " + response.body.substr(0, 80));
        deliver(reject);
        return;
    }
    const json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_object()) exchange_ids_[request.order_id] = parsed.value("order_id", "");
    deliver(base_event(GatewayEventType::Accepted, request.order_id));
}

void KalshiRestGateway::cancel(const OrderRequest& request) {
    const auto it = exchange_ids_.find(request.order_id);
    if (it == exchange_ids_.end()) {
        GatewayEvent reject = base_event(GatewayEventType::CancelRejected, request.order_id);
        copy_reason(reject, "unknown exchange order id");
        deliver(reject);
        return;
    }
    const HttpResponse response = rest_.signed_delete("/portfolio/events/orders/" + it->second);
    if (response.ok()) {
        deliver(base_event(GatewayEventType::Canceled, request.order_id));
    } else if (response.status != 0) {
        GatewayEvent reject = base_event(GatewayEventType::CancelRejected, request.order_id);
        copy_reason(reject, "http " + std::to_string(response.status));
        deliver(reject);
    }
    // status 0: unknown outcome; timeout -> Unknown -> reconcile.
}

void KalshiRestGateway::amend(const OrderRequest& request) {
    const auto it = exchange_ids_.find(request.order_id);
    if (it == exchange_ids_.end()) return;
    const std::string client_id = make_client_order_id(config_.session_id, request.order_id);
    const json body = {{"ticker", ticker_of(request.instrument)},
                       {"side", request.side == Side::Buy ? "bid" : "ask"},
                       {"price", to_string(request.price)},
                       {"count", to_string(request.qty)},
                       {"client_order_id", client_id},
                       {"updated_client_order_id", client_id}};
    const HttpResponse response = rest_.signed_post("/portfolio/events/orders/" + it->second + "/amend", body.dump());
    GatewayEvent event = base_event(response.ok() ? GatewayEventType::Replaced : GatewayEventType::CancelRejected,
                                    request.order_id);
    event.price = request.price;
    event.qty = request.qty;
    if (response.status != 0) deliver(event);
}

void KalshiRestGateway::on_ws_message(const std::string& text) {
    // Runs on the WebSocket thread: translate and queue for poll().
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("msg") || !root["msg"].is_object()) return;
    const std::string type = root.value("type", "");
    const json& msg = root["msg"];
    const OrderId id = parse_client_order_id(msg.value("client_order_id", ""), config_.session_id);
    if (id == 0) return;  // not one of this session's orders

    GatewayEvent event;
    event.order_id = id;
    event.receive_ns = wall_clock_ns();
    if (type == "fill") {
        event.type = GatewayEventType::Fill;
        const std::string trade_id = msg.value("trade_id", "");
        event.fill_id = hash_string(trade_id.data(), trade_id.size());
        event.price = price_field(msg, "yes_price_dollars").value_or(Price{});
        event.qty = qty_field(msg, "count_fp").value_or(Qty{});
        event.is_taker = msg.value("is_taker", false);
        if (msg.contains("fee_cost") && msg["fee_cost"].is_string()) {
            event.fee = parse_scaled_micro(msg["fee_cost"].get<std::string>());
        }
    } else if (type == "user_order") {
        const std::string status = msg.value("status", "");
        if (status == "resting") event.type = GatewayEventType::Accepted;
        else if (status == "canceled") event.type = GatewayEventType::Canceled;
        else return;  // "executed" is fully described by the fill messages
    } else {
        return;
    }
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    inbox_.push_back(event);
}

void KalshiRestGateway::poll(int64_t /*now_ns*/) {
    std::vector<GatewayEvent> events;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        events.swap(inbox_);
    }
    for (GatewayEvent& event : events) {
        if (auto it = instruments_by_order_.find(event.order_id); it != instruments_by_order_.end()) {
            event.instrument = it->second;
        }
        deliver(event);
    }
}

void KalshiRestGateway::reconcile(const std::vector<OrderId>& order_ids) {
    // GET /portfolio/orders can't filter by client_order_id: list this session's orders and match locally.
    const HttpResponse response =
        rest_.signed_get("/portfolio/orders?limit=1000&min_ts=" + std::to_string(started_ns_ / kNanosPerSecond - 60));
    if (!response.ok()) return;  // still Unknown; the engine asks again after the next timeout
    const json root = json::parse(response.body, nullptr, false);
    std::map<OrderId, json> by_id;
    if (root.is_object() && root.contains("orders") && root["orders"].is_array()) {
        for (const json& order : root["orders"]) {
            const OrderId id = parse_client_order_id(order.value("client_order_id", ""), config_.session_id);
            if (id != 0) by_id[id] = order;
        }
    }
    for (const OrderId id : order_ids) {
        GatewayEvent report = base_event(GatewayEventType::StatusReport, id);
        const auto it = by_id.find(id);
        if (it == by_id.end()) {
            report.status = ExchangeOrderStatus::NotFound;
        } else {
            const json& order = it->second;
            const std::string status = order.value("status", "");
            report.status = status == "resting"    ? ExchangeOrderStatus::Resting
                            : status == "executed" ? ExchangeOrderStatus::Filled
                                                   : ExchangeOrderStatus::Canceled;
            report.cumulative_filled = qty_field(order, "fill_count_fp").value_or(Qty{});
            report.price = price_field(order, "yes_price_dollars").value_or(Price{});
            exchange_ids_[id] = order.value("order_id", "");
        }
        deliver(report);
    }
}

void KalshiRestGateway::cancel_all() {
    if (!order_group_id_.empty()) rest_.signed_post("/portfolio/order_groups/" + order_group_id_ + "/trigger", "{}");
    rest_.signed_delete("/portfolio/events/orders");
}

void KalshiRestGateway::stop() { socket_.close(); }

MicroDollars parse_scaled_micro(const std::string& text) {
    MicroDollars whole = 0, fraction = 0;
    int fraction_digits = 0;
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && text[i] == '-') {
        negative = true;
        ++i;
    }
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) whole = whole * 10 + (text[i] - '0');
    if (i < text.size() && text[i] == '.') {
        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9' && fraction_digits < 6; ++i, ++fraction_digits) {
            fraction = fraction * 10 + (text[i] - '0');
        }
    }
    while (fraction_digits < 6) {
        fraction *= 10;
        ++fraction_digits;
    }
    const MicroDollars value = whole * 1'000'000 + fraction;
    return negative ? -value : value;
}

}  // namespace mt
