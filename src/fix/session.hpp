#pragma once
// FIX 4.4 session layer: Logon, Heartbeat/TestRequest, sequence numbers, ResendRequest, GapFill, Logout.
// No transport or clock of its own: on_bytes()/on_timer() in, callbacks.send_bytes() out.
// Single-threaded; callbacks run inside the triggering call, so don't call on_bytes() from one.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "fix/codec.hpp"
#include "fix/sequence_store.hpp"

namespace mt::fix {

enum class Role : uint8_t { Initiator, Acceptor };

enum class SessionState : uint8_t {
    Disconnected,  // no logon on the current connection (an acceptor waits here for a Logon)
    LogonSent,     // initiator sent Logon and waits for the reply
    Active,        // logged on: application messages flow both ways
    LogoutSent,    // we sent Logout and wait for the other side's Logout
    LoggedOut,     // finished (clean logout or fatal error): the transport should close
};

const char* to_string(SessionState state);

enum class Direction : uint8_t { Inbound, Outbound };

struct SessionConfig {
    Role role = Role::Initiator;
    std::string sender_comp_id;  // SenderCompID(49) we put on our messages
    std::string target_comp_id;  // TargetCompID(56): who we talk to
    // HeartBtInt(108): proposed by the initiator, adopted by the acceptor.
    int heartbeat_interval_seconds = 30;
    // Initiator only: send ResetSeqNumFlag(141)=Y and start both sides at 1.
    bool reset_seq_num_on_logon = false;
    // Sequence number file; "" = memory only.
    std::string sequence_file_path;
    int64_t logon_timeout_ns = 10'000'000'000;   // give up if no Logon reply in time
    int64_t logout_timeout_ns = 2'000'000'000;   // close anyway if no Logout reply in time
};

struct SessionCallbacks {
    // Required: put these bytes on the wire (one complete FIX message).
    std::function<void(std::string_view bytes)> send_bytes;
    // Required: application messages, once each, in sequence order. Views into the receive buffer.
    std::function<void(const Message& message)> on_app_message;
    // Optional: every complete message in or out, for wire logs.
    std::function<void(Direction direction, std::string_view raw)> on_wire_message;
    // Optional: session events worth a log line (gap detected, logon, ...).
    std::function<void(std::string_view text)> on_log;
};

class Session {
public:
    // Loads saved sequence numbers if present; throws if the sequence file can't be opened.
    Session(SessionConfig config, SessionCallbacks callbacks);

    // Initiator: send Logon. Call once the TCP connection is up.
    void start_logon(int64_t now_ns);
    // Bytes read from the transport, in any chunking.
    void on_bytes(std::string_view bytes, int64_t now_ns);
    // Heartbeats, TestRequest, logon/logout timeouts, resend retries.
    void on_timer(int64_t now_ns);

    // Sends pre-encoded body fields and returns their MsgSeqNum. Stored for resends: if not Active the
    // number is still used and the peer gets it by resend after the next logon (check is_active()).
    uint64_t send_app_message(std::string_view msg_type, std::string_view body_fields, int64_t now_ns);

    // State becomes LoggedOut when the peer answers or logout_timeout_ns passes.
    void start_logout(std::string_view reason, int64_t now_ns);
    // Keeps sequence numbers and the resend store so a reconnect continues the session.
    void on_disconnected();

    // Test hook: number and store the next app message but don't send it, forcing a gap on the peer.
    void drop_next_outgoing_app_message() { drop_next_outgoing_app_message_ = true; }

    SessionState state() const { return state_; }
    bool is_active() const { return state_ == SessionState::Active; }
    uint64_t next_outgoing_seq() const { return next_outgoing_; }
    uint64_t next_expected_incoming_seq() const { return next_incoming_; }
    int heartbeat_interval_seconds() const { return heartbeat_interval_seconds_; }
    bool resend_in_progress() const { return resend_requested_; }
    size_t queued_message_count() const { return queued_.size(); }
    const SessionConfig& config() const { return config_; }

private:
    // Off the wire, or replayed from the queue of messages that arrived ahead of a gap.
    enum class Arrival : uint8_t { Live, FromQueue };

    struct SentMessage {
        std::string msg_type;
        std::string body;          // application fields, without the header
        int64_t sending_time_ns;   // becomes OrigSendingTime(122) on a resend
    };

    // Inbound
    void handle_inbound(const Message& message, int64_t now_ns);
    void handle_logon(const Message& message, uint64_t seq, int64_t now_ns);
    void handle_seq_too_high(const Message& message, uint64_t seq, int64_t now_ns);
    void handle_seq_too_low(const Message& message, uint64_t seq, int64_t now_ns);
    void process_in_order(const Message& message, Arrival arrival, int64_t now_ns);
    void process_queued_messages(int64_t now_ns);
    void apply_gap_fill(const Message& message, int64_t now_ns);
    void apply_sequence_reset(const Message& message, int64_t now_ns);
    void handle_logout(const Message& message, int64_t now_ns);
    void answer_resend_request(const Message& request, int64_t now_ns);
    bool has_expected_comp_ids(const Message& message) const;
    void advance_incoming();

    // Outbound
    uint64_t send_new_message(std::string_view type, std::string_view body, int64_t now_ns,
                              bool is_application);
    void transmit(std::string_view type, uint64_t seq, std::string_view body, int64_t now_ns,
                  std::optional<int64_t> orig_sending_time_ns);
    void send_logon(int64_t now_ns, bool reset_seq_num);
    void send_heartbeat(int64_t now_ns, std::string_view test_request_id);
    void send_test_request(int64_t now_ns);
    void send_resend_request(int64_t now_ns);
    void send_gap_fill(uint64_t seq, uint64_t new_seq, int64_t now_ns);
    void send_logout(std::string_view text, int64_t now_ns);
    void send_reject(uint64_t ref_seq, std::string_view text, int64_t now_ns);
    void store_sent_message(uint64_t seq, std::string_view type, std::string_view body, int64_t now_ns);

    // Logs `reason`, sends Logout with it, and ends the session.
    void fail_session(const std::string& reason, int64_t now_ns);
    void check_heartbeat_timers(int64_t now_ns);
    void reset_sequence_numbers();
    void persist_sequence_numbers();
    void log(const std::string& text) const;

    SessionConfig config_;
    SessionCallbacks callbacks_;
    SequenceStore store_;

    SessionState state_ = SessionState::Disconnected;
    int heartbeat_interval_seconds_ = 30;
    uint64_t next_outgoing_ = 1;  // MsgSeqNum for our next new message
    uint64_t next_incoming_ = 1;  // MsgSeqNum we expect next from the other side

    // Timers (all wall-clock ns from the caller)
    int64_t last_sent_ns_ = 0;
    int64_t last_received_ns_ = 0;
    int64_t logon_sent_ns_ = 0;
    int64_t logout_sent_ns_ = 0;
    bool test_request_pending_ = false;
    int64_t test_request_sent_ns_ = 0;
    uint64_t test_request_counter_ = 0;

    // Gap recovery: messages past a gap wait in queued_ until a resend or gap fill closes it.
    bool resend_requested_ = false;
    uint64_t resend_until_seq_ = 0;  // highest MsgSeqNum seen when the gap opened
    int64_t resend_request_sent_ns_ = 0;
    std::map<uint64_t, std::string> queued_;  // seq -> raw bytes of a message that arrived early

    // Resend store: application messages by MsgSeqNum. Admin messages aren't kept; resends gap-fill them.
    std::map<uint64_t, SentMessage> sent_messages_;

    bool drop_next_outgoing_app_message_ = false;

    StreamDecoder decoder_;
    Message inbound_;         // the message being processed from decoder_
    Message queued_message_;  // a message replayed from queued_
    MessageBuilder builder_;
    std::string admin_body_;  // reused body buffer for admin messages
};

}  // namespace mt::fix
