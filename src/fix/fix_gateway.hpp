#pragma once
// FixGateway: OrderGateway speaking FIX 4.4 over TCP to the mock exchange. Socket work runs in poll()
// on the gateway thread and never blocks; only start() and stop() wait (bounded).
// ClOrdIDs: "mt1-42" for the new order, "mt1-42.c17" / "mt1-42.r18" for cancel / replace.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/instruments.hpp"
#include "fix/session.hpp"
#include "fix/tcp.hpp"
#include "gw/order_gateway.hpp"

namespace mt {

struct FixGatewayConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9878;
    std::string sender_comp_id = "MILLITRADER";
    std::string target_comp_id = "MOCKEX";
    int heartbeat_interval_seconds = 30;
    std::string sequence_file_path = "data/fix/MILLITRADER-MOCKEX.seq";  // "" = memory only
    uint32_t session_id = 1;              // goes into every ClOrdID
    bool reset_seq_num_on_logon = false;  // ResetSeqNumFlag=Y: start both sides at 1
    int connect_timeout_ms = 2000;
    bool log_messages = false;            // print every FIX message in/out to stderr
    bool log_session_events = true;       // print logon, gaps, resends, disconnects ... to stderr
};

class FixGateway : public OrderGateway {
public:
    FixGateway(FixGatewayConfig config, const InstrumentTable& instruments);
    ~FixGateway() override;

    const char* name() const override { return "fix"; }
    bool start(GatewayEventSink& sink) override;
    void send(const OrderRequest& request) override;
    void poll(int64_t now_ns) override;  // now_ns: wall clock
    void reconcile(const std::vector<OrderId>& order_ids) override;
    void cancel_all() override;
    void stop() override;

    // Starts the Logout handshake without waiting; later poll() calls finish it.
    void request_logout();

    bool is_logged_on() const { return session_ && session_->is_active(); }
    bool is_connected() const { return connection_ && connection_->is_open(); }
    const fix::Session* session() const { return session_.get(); }
    size_t open_order_count() const;

private:
    // Per-order state so cancels/replaces name the right OrigClOrdID and cancel_all knows what's open.
    struct TrackedOrder {
        InstrumentId instrument = kNoInstrument;
        Side side = Side::Buy;
        bool post_only = true;
        Price price{};
        Qty qty{};
        Qty cumulative_filled{};
        std::string cl_ord_id;   // latest ClOrdID the exchange acknowledged
        bool open = false;       // acknowledged and not yet filled or canceled
        bool cancel_pending = false;
    };

    void send_now(const OrderRequest& request);
    void send_new_order(const OrderRequest& request);
    void send_cancel(const OrderRequest& request);
    void send_replace(const OrderRequest& request);
    void send_status_request(OrderId order_id);
    void send_queued_requests();
    void reject_locally(const OrderRequest& request, std::string_view reason);

    void on_app_message(const fix::Message& message);
    void handle_execution_report(const fix::Message& message);
    void handle_cancel_reject(const fix::Message& message);
    GatewayEvent make_event(GatewayEventType type, OrderId order_id, const fix::Message& message) const;
    void emit(const GatewayEvent& event);

    void read_socket();
    void close_if_session_over();
    bool is_logon_pending() const;
    OrderId order_id_from_cl_ord_id(std::string_view cl_ord_id) const;
    std::string next_request_cl_ord_id(OrderId order_id, char kind) const;
    std::string_view symbol_for(InstrumentId instrument) const;
    void log(const std::string& text) const;         // if log_session_events
    void log_always(const std::string& text) const;  // errors from start(), wire logs

    FixGatewayConfig config_;
    const InstrumentTable& instruments_;
    GatewayEventSink* sink_ = nullptr;
    std::unique_ptr<fix::Session> session_;
    std::unique_ptr<fix::TcpConnection> connection_;
    int64_t now_ns_ = 0;  // latest time from poll(), used for SendingTime/TransactTime

    std::unordered_map<OrderId, TrackedOrder> orders_;
    std::vector<OrderRequest> queued_requests_;  // arrived while Logon was in flight
    std::vector<OrderId> queued_status_requests_;
    uint64_t status_request_counter_ = 0;

    std::string body_;                  // reused application-body buffer
    std::vector<char> receive_chunk_;   // reused socket read buffer
};

// FNV-1a of an ExecID, used as fill_id so the engine can drop duplicate fills (e.g. resent reports).
uint64_t hash_exec_id(std::string_view exec_id);

}  // namespace mt
