#pragma once
// OrderGateway: interface every venue sits behind (paper, Kalshi demo REST, FIX 4.4 mock exchange).
// Methods and sink callbacks run on the gateway thread, the single producer of the engine's queue,
// so a gateway with its own I/O threads must hand events to that thread first.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/events.hpp"

namespace mt {

class GatewayEventSink {
public:
    virtual ~GatewayEventSink() = default;
    virtual void on_gateway_event(const GatewayEvent& event) = 0;
};

class OrderGateway {
public:
    virtual ~OrderGateway() = default;

    virtual const char* name() const = 0;

    // Connect / log on. Returns false if the venue cannot be reached.
    virtual bool start(GatewayEventSink& sink) = 0;

    // New, cancel or replace.
    virtual void send(const OrderRequest& request) = 0;

    // Called in a loop on the gateway thread: read sockets, run timers, deliver events. Must not block long.
    virtual void poll(int64_t now_ns) = 0;

    // Ask the venue about these orders and answer each with a StatusReport. Used after a timeout or
    // restart: an Unknown order is settled by asking, never by resending.
    virtual void reconcile(const std::vector<OrderId>& order_ids) = 0;

    // Cancel everything this session has open (kill switch).
    virtual void cancel_all() = 0;

    virtual void stop() = 0;
};

// "mt<session>-<order id>", built before the send so a timed-out order can be found by it.
std::string make_client_order_id(uint32_t session_id, OrderId order_id);
// Returns 0 if the id is not one of ours.
OrderId parse_client_order_id(std::string_view client_order_id, uint32_t session_id);

}  // namespace mt
