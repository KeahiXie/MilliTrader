#include "fix/fix_gateway.hpp"

#include <poll.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>

#include "core/clock.hpp"
#include "fix/fix_tags.hpp"

namespace mt {

namespace {

namespace tag = fix::tag;
namespace msg_type = fix::msg_type;

// Enough reads per poll() to drain a burst without starving the session timers.
constexpr int kMaxReadsPerPoll = 8;
constexpr size_t kReceiveChunkBytes = 64 * 1024;

void copy_reason(std::array<char, 40>& out, std::string_view text) {
    // copy_n, not memcpy: an absent field's data() may be null, and memcpy(dst, nullptr, 0) is UB.
    const size_t length = std::min(text.size(), out.size() - 1);
    std::copy_n(text.begin(), length, out.begin());
    out[length] = '\0';
}

char side_code(Side side) { return side == Side::Buy ? fix::side_code::Buy : fix::side_code::Sell; }

ExchangeOrderStatus to_exchange_status(char ord_status) {
    switch (ord_status) {
        case fix::ord_status::New:
        case fix::ord_status::PartiallyFilled: return ExchangeOrderStatus::Resting;
        case fix::ord_status::Filled: return ExchangeOrderStatus::Filled;
        case fix::ord_status::Canceled:
        case fix::ord_status::Expired: return ExchangeOrderStatus::Canceled;
        default: return ExchangeOrderStatus::NotFound;  // Rejected = "no such order"
    }
}

bool is_open_status(char ord_status) {
    return ord_status == fix::ord_status::New || ord_status == fix::ord_status::PartiallyFilled;
}

}  // namespace

uint64_t hash_exec_id(std::string_view exec_id) {
    uint64_t hash = 14695981039346656037ull;  // FNV-1a 64-bit offset basis
    for (const char c : exec_id) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;  // FNV prime
    }
    return hash;
}

FixGateway::FixGateway(FixGatewayConfig config, const InstrumentTable& instruments)
    : config_(std::move(config)), instruments_(instruments), receive_chunk_(kReceiveChunkBytes) {}

FixGateway::~FixGateway() = default;  // closes the socket; call stop() first for a clean Logout

bool FixGateway::start(GatewayEventSink& sink) {
    sink_ = &sink;
    now_ns_ = wall_clock_ns();

    if (!session_) {
        if (!config_.sequence_file_path.empty()) {
            const std::filesystem::path parent = std::filesystem::path(config_.sequence_file_path).parent_path();
            std::error_code ignored;
            if (!parent.empty()) std::filesystem::create_directories(parent, ignored);
        }
        fix::SessionConfig session_config;
        session_config.role = fix::Role::Initiator;
        session_config.sender_comp_id = config_.sender_comp_id;
        session_config.target_comp_id = config_.target_comp_id;
        session_config.heartbeat_interval_seconds = config_.heartbeat_interval_seconds;
        session_config.reset_seq_num_on_logon = config_.reset_seq_num_on_logon;
        session_config.sequence_file_path = config_.sequence_file_path;

        fix::SessionCallbacks callbacks;
        callbacks.send_bytes = [this](std::string_view bytes) {
            if (connection_) connection_->write(bytes);
        };
        callbacks.on_app_message = [this](const fix::Message& message) { on_app_message(message); };
        if (config_.log_messages) {
            callbacks.on_wire_message = [this](fix::Direction direction, std::string_view raw) {
                log_always((direction == fix::Direction::Inbound ? "IN  " : "OUT ") + fix::to_printable(raw));
            };
        }
        if (config_.log_session_events) {
            callbacks.on_log = [this](std::string_view text) { log(std::string(text)); };
        }
        try {
            session_ = std::make_unique<fix::Session>(session_config, callbacks);
        } catch (const std::exception& error) {
            log_always(error.what());
            return false;
        }
    }

    if (is_connected()) return true;
    std::string error;
    const int fd = fix::connect_tcp(config_.host, config_.port, config_.connect_timeout_ms, error);
    if (fd < 0) {
        log_always(error);
        return false;
    }
    connection_ = std::make_unique<fix::TcpConnection>(fd);
    session_->start_logon(now_ns_);
    return true;
}

void FixGateway::send(const OrderRequest& request) {
    if (is_logged_on()) {
        send_now(request);
    } else if (is_logon_pending()) {
        queued_requests_.push_back(request);  // goes out as soon as the Logon reply arrives
    } else if (request.type == OrderRequestType::Reconcile) {
        log("cannot reconcile order " + std::to_string(request.order_id) + ": not connected");
    } else {
        reject_locally(request, "fix session not connected");
    }
}

void FixGateway::poll(int64_t now_ns) {
    now_ns_ = now_ns;
    if (!session_) return;

    if (is_connected()) {
        pollfd entry{connection_->fd(), POLLIN, 0};
        if (connection_->has_pending_output()) entry.events |= POLLOUT;
        // Zero timeout: never blocks the gateway thread.
        if (::poll(&entry, 1, 0) > 0) {
            if (entry.revents & POLLOUT) connection_->flush();
            if (entry.revents & (POLLIN | POLLHUP | POLLERR)) read_socket();
        }
    }
    session_->on_timer(now_ns);
    if (is_logged_on()) send_queued_requests();
    close_if_session_over();
}

void FixGateway::reconcile(const std::vector<OrderId>& order_ids) {
    for (const OrderId order_id : order_ids) {
        if (is_logged_on()) {
            send_status_request(order_id);
        } else if (is_logon_pending()) {
            queued_status_requests_.push_back(order_id);
        } else {
            log("cannot reconcile order " + std::to_string(order_id) + ": not connected");
        }
    }
}

void FixGateway::cancel_all() {
    // Collect first: send() can emit events, and a sink is allowed to look at us.
    std::vector<OrderRequest> cancels;
    for (const auto& [order_id, order] : orders_) {
        if (!order.open || order.cancel_pending) continue;
        OrderRequest cancel;
        cancel.type = OrderRequestType::Cancel;
        cancel.order_id = order_id;
        cancel.instrument = order.instrument;
        cancel.side = order.side;
        cancels.push_back(cancel);
    }
    for (const OrderRequest& cancel : cancels) send(cancel);
}

void FixGateway::stop() {
    now_ns_ = wall_clock_ns();
    request_logout();
    // Bounded wait for the Logout reply (shutdown only).
    const int64_t deadline = wall_clock_ns() + kNanosPerSecond;
    while (is_connected() && wall_clock_ns() < deadline) {
        pollfd entry{connection_->fd(), POLLIN, 0};
        ::poll(&entry, 1, 20);
        poll(wall_clock_ns());
    }
    if (connection_) {
        connection_.reset();
        session_->on_disconnected();
    }
}

void FixGateway::request_logout() {
    if (session_) session_->start_logout("client logging out", now_ns_);
}

size_t FixGateway::open_order_count() const {
    size_t count = 0;
    for (const auto& entry : orders_) count += entry.second.open ? 1 : 0;
    return count;
}

void FixGateway::send_now(const OrderRequest& request) {
    switch (request.type) {
        case OrderRequestType::New: send_new_order(request); break;
        case OrderRequestType::Cancel: send_cancel(request); break;
        case OrderRequestType::Replace: send_replace(request); break;
        case OrderRequestType::Reconcile: send_status_request(request.order_id); break;
    }
}

void FixGateway::send_new_order(const OrderRequest& request) {
    const std::string_view symbol = symbol_for(request.instrument);
    if (symbol.empty()) {
        reject_locally(request, "unknown instrument");
        return;
    }
    TrackedOrder order;
    order.instrument = request.instrument;
    order.side = request.side;
    order.post_only = request.post_only;
    order.price = request.price;
    order.qty = request.qty;
    order.cl_ord_id = make_client_order_id(config_.session_id, request.order_id);

    body_.clear();
    fix::append_field(body_, tag::ClOrdID, order.cl_ord_id);
    fix::append_field(body_, tag::Symbol, symbol);
    fix::append_char_field(body_, tag::Side, side_code(request.side));
    fix::append_field(body_, tag::OrderQty, to_string(request.qty));
    fix::append_char_field(body_, tag::OrdType, fix::ord_type::Limit);
    fix::append_field(body_, tag::Price, to_string(request.price));
    fix::append_char_field(body_, tag::TimeInForce, fix::time_in_force::Day);
    if (request.post_only) fix::append_char_field(body_, tag::ExecInst, fix::kExecInstPostOnly);
    fix::append_timestamp_field(body_, tag::TransactTime, now_ns_);

    orders_[request.order_id] = std::move(order);
    session_->send_app_message(msg_type::NewOrderSingle, body_, now_ns_);
}

void FixGateway::send_cancel(const OrderRequest& request) {
    InstrumentId instrument = request.instrument;
    Side side = request.side;
    // Unknown order (placed before a restart): its first ClOrdID is predictable, so still try.
    std::string orig_cl_ord_id = make_client_order_id(config_.session_id, request.order_id);
    auto tracked = orders_.find(request.order_id);
    if (tracked != orders_.end()) {
        instrument = tracked->second.instrument;
        side = tracked->second.side;
        orig_cl_ord_id = tracked->second.cl_ord_id;
        tracked->second.cancel_pending = true;
    }

    body_.clear();
    fix::append_field(body_, tag::OrigClOrdID, orig_cl_ord_id);
    fix::append_field(body_, tag::ClOrdID, next_request_cl_ord_id(request.order_id, 'c'));
    const std::string_view symbol = symbol_for(instrument);
    if (!symbol.empty()) fix::append_field(body_, tag::Symbol, symbol);
    fix::append_char_field(body_, tag::Side, side_code(side));
    fix::append_timestamp_field(body_, tag::TransactTime, now_ns_);
    session_->send_app_message(msg_type::OrderCancelRequest, body_, now_ns_);
}

void FixGateway::send_replace(const OrderRequest& request) {
    auto tracked = orders_.find(request.order_id);
    const bool known = tracked != orders_.end();
    const InstrumentId instrument = known ? tracked->second.instrument : request.instrument;
    const Side side = known ? tracked->second.side : request.side;
    const bool post_only = known ? tracked->second.post_only : request.post_only;
    const std::string orig_cl_ord_id =
        known ? tracked->second.cl_ord_id : make_client_order_id(config_.session_id, request.order_id);

    // Keep the tracked ClOrdID until the replace is confirmed, so a cancel in between names the live order.
    body_.clear();
    fix::append_field(body_, tag::OrigClOrdID, orig_cl_ord_id);
    fix::append_field(body_, tag::ClOrdID, next_request_cl_ord_id(request.order_id, 'r'));
    const std::string_view symbol = symbol_for(instrument);
    if (!symbol.empty()) fix::append_field(body_, tag::Symbol, symbol);
    fix::append_char_field(body_, tag::Side, side_code(side));
    fix::append_field(body_, tag::OrderQty, to_string(request.qty));
    fix::append_char_field(body_, tag::OrdType, fix::ord_type::Limit);
    fix::append_field(body_, tag::Price, to_string(request.price));
    fix::append_char_field(body_, tag::TimeInForce, fix::time_in_force::Day);
    if (post_only) fix::append_char_field(body_, tag::ExecInst, fix::kExecInstPostOnly);
    fix::append_timestamp_field(body_, tag::TransactTime, now_ns_);
    session_->send_app_message(msg_type::OrderCancelReplaceRequest, body_, now_ns_);
}

void FixGateway::send_status_request(OrderId order_id) {
    auto tracked = orders_.find(order_id);
    body_.clear();
    fix::append_field(body_, tag::ClOrdID,
                      tracked != orders_.end() ? tracked->second.cl_ord_id
                                               : make_client_order_id(config_.session_id, order_id));
    fix::append_field(body_, tag::OrdStatusReqID, "S" + std::to_string(++status_request_counter_));
    if (tracked != orders_.end()) {
        const std::string_view symbol = symbol_for(tracked->second.instrument);
        if (!symbol.empty()) fix::append_field(body_, tag::Symbol, symbol);
        fix::append_char_field(body_, tag::Side, side_code(tracked->second.side));
    }
    session_->send_app_message(msg_type::OrderStatusRequest, body_, now_ns_);
}

void FixGateway::send_queued_requests() {
    if (queued_requests_.empty() && queued_status_requests_.empty()) return;
    std::vector<OrderRequest> requests;
    requests.swap(queued_requests_);
    for (const OrderRequest& request : requests) send_now(request);
    std::vector<OrderId> status_requests;
    status_requests.swap(queued_status_requests_);
    for (const OrderId order_id : status_requests) send_status_request(order_id);
}

void FixGateway::reject_locally(const OrderRequest& request, std::string_view reason) {
    GatewayEvent event;
    event.type = request.type == OrderRequestType::New ? GatewayEventType::Rejected
                                                       : GatewayEventType::CancelRejected;
    event.order_id = request.order_id;
    event.instrument = request.instrument;
    event.side = request.side;
    event.receive_ns = now_ns_;
    copy_reason(event.reason, reason);
    emit(event);
}

void FixGateway::on_app_message(const fix::Message& message) {
    const std::string_view type = message.msg_type();
    if (type == msg_type::ExecutionReport) {
        handle_execution_report(message);
    } else if (type == msg_type::OrderCancelReject) {
        handle_cancel_reject(message);
    } else if (type == msg_type::BusinessMessageReject) {
        log("exchange rejected our message " + std::string(message.get(tag::RefSeqNum)) + ": " +
            std::string(message.get(tag::Text)));
    } else {
        log("ignoring application message type " + std::string(type));
    }
}

void FixGateway::handle_execution_report(const fix::Message& message) {
    OrderId order_id = order_id_from_cl_ord_id(message.get(tag::ClOrdID));
    if (order_id == 0) order_id = order_id_from_cl_ord_id(message.get(tag::OrigClOrdID));
    if (order_id == 0) {
        log("ExecutionReport for an order that is not ours: ClOrdID " + std::string(message.get(tag::ClOrdID)));
        return;
    }

    GatewayEvent event = make_event(GatewayEventType::Accepted, order_id, message);
    const Price order_price = parse_price(message.get(tag::Price)).value_or(Price{});
    const Qty order_qty = parse_qty(message.get(tag::OrderQty)).value_or(Qty{});
    event.price = order_price;
    event.qty = order_qty;
    event.cumulative_filled = parse_qty(message.get(tag::CumQty)).value_or(Qty{});
    const char ord_status = message.get_char(tag::OrdStatus);

    switch (message.get_char(tag::ExecType)) {
        case fix::exec_type::New:
            event.type = GatewayEventType::Accepted;
            break;
        case fix::exec_type::Trade:
            event.type = GatewayEventType::Fill;
            event.price = parse_price(message.get(tag::LastPx)).value_or(Price{});
            event.qty = parse_qty(message.get(tag::LastQty)).value_or(Qty{});
            event.fill_id = hash_exec_id(message.get(tag::ExecID));
            event.is_taker = message.get_char(tag::LastLiquidityInd) == fix::kLiquidityRemoved;
            break;
        case fix::exec_type::Canceled:
        case fix::exec_type::Expired:
            event.type = GatewayEventType::Canceled;
            break;
        case fix::exec_type::Replaced:
            event.type = GatewayEventType::Replaced;  // price = new price, qty = new total qty
            break;
        case fix::exec_type::Rejected:
            event.type = GatewayEventType::Rejected;
            copy_reason(event.reason, message.get(tag::Text));
            break;
        case fix::exec_type::OrderStatus:
            // StatusReport: qty = order qty, cumulative_filled = filled so far.
            event.type = GatewayEventType::StatusReport;
            event.status = to_exchange_status(ord_status);
            copy_reason(event.reason, message.get(tag::Text));
            break;
        default:
            log("ignoring ExecutionReport with ExecType " + std::string(message.get(tag::ExecType)));
            return;
    }

    // Keep tracking only what can still be canceled.
    if (is_open_status(ord_status)) {
        TrackedOrder& order = orders_[order_id];
        order.instrument = event.instrument;
        order.side = event.side;
        order.price = order_price;
        order.qty = order_qty;
        order.cumulative_filled = event.cumulative_filled;
        order.cl_ord_id = std::string(message.get(tag::ClOrdID));  // the exchange reports the current one
        order.open = true;
    } else {
        orders_.erase(order_id);
    }
    emit(event);
}

void FixGateway::handle_cancel_reject(const fix::Message& message) {
    OrderId order_id = order_id_from_cl_ord_id(message.get(tag::OrigClOrdID));
    if (order_id == 0) order_id = order_id_from_cl_ord_id(message.get(tag::ClOrdID));
    if (order_id == 0) {
        log("OrderCancelReject for an order that is not ours");
        return;
    }
    GatewayEvent event = make_event(GatewayEventType::CancelRejected, order_id, message);
    copy_reason(event.reason, message.get(tag::Text));

    auto tracked = orders_.find(order_id);
    if (tracked != orders_.end()) {
        tracked->second.cancel_pending = false;
        // "Too late" because it already filled or was canceled: nothing left to track.
        if (!is_open_status(message.get_char(tag::OrdStatus))) orders_.erase(tracked);
    }
    emit(event);
}

GatewayEvent FixGateway::make_event(GatewayEventType type, OrderId order_id, const fix::Message& message) const {
    GatewayEvent event;
    event.type = type;
    event.order_id = order_id;
    event.receive_ns = now_ns_;

    auto tracked = orders_.find(order_id);
    if (tracked != orders_.end()) {
        event.instrument = tracked->second.instrument;
        event.side = tracked->second.side;
    } else {
        event.instrument = instruments_.find_id(Venue::Fix, message.get(tag::Symbol));
    }
    const char side = message.get_char(tag::Side);
    if (side == fix::side_code::Buy) event.side = Side::Buy;
    if (side == fix::side_code::Sell) event.side = Side::Sell;
    return event;
}

void FixGateway::emit(const GatewayEvent& event) {
    if (sink_) sink_->on_gateway_event(event);
}

void FixGateway::read_socket() {
    for (int read = 0; read < kMaxReadsPerPoll; ++read) {
        const fix::TcpConnection::ReadResult result =
            connection_->read_some(receive_chunk_.data(), receive_chunk_.size());
        if (result.status == fix::TcpConnection::ReadStatus::WouldBlock) return;
        if (result.status == fix::TcpConnection::ReadStatus::Closed) {
            log("exchange closed the connection");
            connection_->close();
            return;
        }
        session_->on_bytes(std::string_view(receive_chunk_.data(), result.size), now_ns_);
        if (!connection_->is_open()) return;
    }
}

void FixGateway::close_if_session_over() {
    if (!connection_) return;
    const bool session_over = session_->state() == fix::SessionState::LoggedOut;
    if (!session_over && connection_->is_open()) return;
    connection_->flush();  // best effort: let a final Logout reach the wire
    connection_.reset();
    session_->on_disconnected();

    // Requests queued for a Logon that never completed must not go out on a later reconnect.
    std::vector<OrderRequest> never_sent;
    never_sent.swap(queued_requests_);
    for (const OrderRequest& request : never_sent) {
        if (request.type != OrderRequestType::Reconcile) reject_locally(request, "fix session closed before logon");
    }
    queued_status_requests_.clear();
    log("disconnected (next outgoing " + std::to_string(session_->next_outgoing_seq()) + ", next expected " +
        std::to_string(session_->next_expected_incoming_seq()) + ")");
}

bool FixGateway::is_logon_pending() const {
    return is_connected() && session_ && session_->state() == fix::SessionState::LogonSent;
}

OrderId FixGateway::order_id_from_cl_ord_id(std::string_view cl_ord_id) const {
    // "mt1-42" or "mt1-42.c17": the order id is the part before the dot.
    return parse_client_order_id(cl_ord_id.substr(0, cl_ord_id.find('.')), config_.session_id);
}

std::string FixGateway::next_request_cl_ord_id(OrderId order_id, char kind) const {
    // Suffix = this request's MsgSeqNum: unique in the session and persisted across restarts.
    return make_client_order_id(config_.session_id, order_id) + "." + kind +
           std::to_string(session_->next_outgoing_seq());
}

std::string_view FixGateway::symbol_for(InstrumentId instrument) const {
    const InstrumentInfo* info = instruments_.find(instrument);
    return info ? info->symbol_view() : std::string_view{};
}

void FixGateway::log(const std::string& text) const {
    if (config_.log_session_events) log_always(text);
}

void FixGateway::log_always(const std::string& text) const {
    std::fprintf(stderr, "[fix %s] %s\n", config_.sender_comp_id.c_str(), text.c_str());
}

}  // namespace mt
