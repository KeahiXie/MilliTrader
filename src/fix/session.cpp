#include "fix/session.hpp"

#include <algorithm>
#include <cassert>

namespace mt::fix {

namespace {

constexpr int64_t kNanosPerSecond = 1'000'000'000;
// Bounds on memory a misbehaving peer can make us use.
constexpr size_t kMaxStoredMessages = 100'000;
constexpr size_t kMaxQueuedMessages = 10'000;

std::string seq_text(uint64_t seq) { return std::to_string(seq); }

}  // namespace

const char* to_string(SessionState state) {
    switch (state) {
        case SessionState::Disconnected: return "disconnected";
        case SessionState::LogonSent: return "logon-sent";
        case SessionState::Active: return "active";
        case SessionState::LogoutSent: return "logout-sent";
        case SessionState::LoggedOut: return "logged-out";
    }
    return "?";
}

Session::Session(SessionConfig config, SessionCallbacks callbacks)
    : config_(std::move(config)),
      callbacks_(std::move(callbacks)),
      store_(config_.sequence_file_path),
      heartbeat_interval_seconds_(config_.heartbeat_interval_seconds) {
    assert(callbacks_.send_bytes && "a session needs somewhere to send bytes");
    if (store_.load(next_outgoing_, next_incoming_)) {
        log("loaded sequence numbers from " + store_.path() + ": next outgoing " +
            seq_text(next_outgoing_) + ", next expected " + seq_text(next_incoming_));
    }
}

void Session::start_logon(int64_t now_ns) {
    if (config_.role != Role::Initiator || state_ != SessionState::Disconnected) return;
    if (config_.reset_seq_num_on_logon) reset_sequence_numbers();
    state_ = SessionState::LogonSent;
    logon_sent_ns_ = now_ns;
    last_received_ns_ = now_ns;
    send_logon(now_ns, config_.reset_seq_num_on_logon);
}

void Session::on_bytes(std::string_view bytes, int64_t now_ns) {
    // Any bytes prove the peer is alive: restart the silence timer.
    last_received_ns_ = now_ns;
    test_request_pending_ = false;

    decoder_.append(bytes);
    while (state_ != SessionState::LoggedOut) {
        const DecodeResult result = decoder_.next(inbound_);
        if (result.status == DecodeStatus::NeedMoreBytes) break;
        if (result.status != DecodeStatus::Ok) {
            // Garbled messages don't use a sequence number; if it was real, the next one shows a gap.
            log(std::string("dropped ") + std::to_string(result.consumed) + " unreadable bytes (" +
                to_string(result.status) + ")");
            continue;
        }
        if (callbacks_.on_wire_message) callbacks_.on_wire_message(Direction::Inbound, inbound_.raw());
        handle_inbound(inbound_, now_ns);
    }
}

void Session::on_timer(int64_t now_ns) {
    switch (state_) {
        case SessionState::LogonSent:
            if (now_ns - logon_sent_ns_ >= config_.logon_timeout_ns) {
                log("no Logon reply; giving up");
                state_ = SessionState::LoggedOut;
            }
            break;
        case SessionState::LogoutSent:
            if (now_ns - logout_sent_ns_ >= config_.logout_timeout_ns) {
                log("no Logout reply; closing anyway");
                state_ = SessionState::LoggedOut;
            }
            break;
        case SessionState::Active:
            check_heartbeat_timers(now_ns);
            break;
        case SessionState::Disconnected:
        case SessionState::LoggedOut:
            break;
    }
}

uint64_t Session::send_app_message(std::string_view msg_type, std::string_view body_fields,
                                   int64_t now_ns) {
    return send_new_message(msg_type, body_fields, now_ns, /*is_application=*/true);
}

void Session::start_logout(std::string_view reason, int64_t now_ns) {
    if (state_ == SessionState::LogonSent) {
        state_ = SessionState::LoggedOut;  // nothing to say goodbye to yet
        return;
    }
    if (state_ != SessionState::Active) return;
    send_logout(reason, now_ns);
    state_ = SessionState::LogoutSent;
    logout_sent_ns_ = now_ns;
}

void Session::on_disconnected() {
    if (state_ != SessionState::Disconnected) log("transport closed");
    state_ = SessionState::Disconnected;
    decoder_.clear();
    // Queued messages sit behind an unfilled gap; the peer resends them after the next logon.
    queued_.clear();
    resend_requested_ = false;
    resend_until_seq_ = 0;
    test_request_pending_ = false;
}

void Session::handle_inbound(const Message& message, int64_t now_ns) {
    if (config_.role == Role::Initiator && state_ == SessionState::Disconnected) {
        log("ignoring a message that arrived before we sent Logon");
        return;
    }
    if (!has_expected_comp_ids(message)) {
        fail_session("CompID problem: got SenderCompID=" + std::string(message.get(tag::SenderCompID)) +
                         " TargetCompID=" + std::string(message.get(tag::TargetCompID)),
                     now_ns);
        return;
    }
    const std::optional<uint64_t> seq = message.get_uint(tag::MsgSeqNum);
    if (!seq || *seq == 0) {
        fail_session("missing or invalid MsgSeqNum", now_ns);
        return;
    }

    if (state_ == SessionState::Disconnected || state_ == SessionState::LogonSent) {
        handle_logon(message, *seq, now_ns);
        return;
    }

    // SequenceReset-Reset (no GapFillFlag) ignores its own MsgSeqNum: just expect NewSeqNo next.
    if (message.msg_type() == msg_type::SequenceReset && !message.get_flag(tag::GapFillFlag)) {
        apply_sequence_reset(message, now_ns);
        return;
    }

    if (*seq > next_incoming_) {
        handle_seq_too_high(message, *seq, now_ns);
    } else if (*seq < next_incoming_) {
        handle_seq_too_low(message, *seq, now_ns);
    } else {
        process_in_order(message, Arrival::Live, now_ns);
        process_queued_messages(now_ns);
    }
}

void Session::handle_logon(const Message& message, uint64_t seq, int64_t now_ns) {
    const std::string_view type = message.msg_type();
    if (type == msg_type::Logout) {
        log("logon refused: " + std::string(message.get(tag::Text)));
        state_ = SessionState::LoggedOut;
        return;
    }
    if (type != msg_type::Logon) {
        fail_session("first message must be Logon, got MsgType " + std::string(type), now_ns);
        return;
    }

    const bool reset_requested = message.get_flag(tag::ResetSeqNumFlag);
    if (config_.role == Role::Acceptor) {
        const std::optional<uint64_t> heartbeat = message.get_uint(tag::HeartBtInt);
        if (!heartbeat || *heartbeat == 0 || *heartbeat > 3600) {
            fail_session("Logon needs a HeartBtInt between 1 and 3600 seconds", now_ns);
            return;
        }
        heartbeat_interval_seconds_ = static_cast<int>(*heartbeat);
        if (reset_requested) {
            log("peer asked for ResetSeqNumFlag=Y: both sides restart at 1");
            reset_sequence_numbers();
        }
    }

    // Too low on Logon: the peer lost its sequence file or is replaying. Accepting would desync history.
    if (seq < next_incoming_) {
        fail_session("MsgSeqNum too low on Logon, expecting " + seq_text(next_incoming_) +
                         " but received " + seq_text(seq),
                     now_ns);
        return;
    }

    if (config_.role == Role::Acceptor) send_logon(now_ns, reset_requested);
    state_ = SessionState::Active;
    last_received_ns_ = now_ns;
    test_request_pending_ = false;
    log("logged on (HeartBtInt " + std::to_string(heartbeat_interval_seconds_) + "s); their Logon is " +
        seq_text(seq) + ", we expected " + seq_text(next_incoming_) + "; our next outgoing is " +
        seq_text(next_outgoing_));

    if (seq > next_incoming_) {
        // We missed messages (e.g. while down): request them. The Logon itself waits in the queue.
        handle_seq_too_high(message, seq, now_ns);
        return;
    }
    advance_incoming();
}

void Session::handle_seq_too_high(const Message& message, uint64_t seq, int64_t now_ns) {
    const std::string_view type = message.msg_type();
    if (type == msg_type::Logout) {
        // Peer is leaving: confirm now; missing messages come back with the resend after the next Logon.
        handle_logout(message, now_ns);
        return;
    }
    if (type == msg_type::ResendRequest) {
        // Answer right away despite our own gap, or both sides could wait on each other forever.
        answer_resend_request(message, now_ns);
    }

    if (queued_.size() >= kMaxQueuedMessages) {
        fail_session("too many messages waiting for a gap to be filled", now_ns);
        return;
    }
    // Keep a copy (the decoder's buffer is reused) and replay it in order later.
    queued_.emplace(seq, std::string(message.raw()));
    resend_until_seq_ = std::max(resend_until_seq_, seq);

    if (!resend_requested_) {
        log("sequence gap: expecting " + seq_text(next_incoming_) + " but received " + seq_text(seq) +
            "; sending ResendRequest " + seq_text(next_incoming_) + "..0");
        send_resend_request(now_ns);
    }
    // An outstanding ResendRequest has EndSeqNo=0 (everything after), so it covers this one too.
}

void Session::handle_seq_too_low(const Message& message, uint64_t seq, int64_t now_ns) {
    if (message.get_flag(tag::PossDupFlag)) {
        // Resent copy of something already processed: normal during recovery.
        return;
    }
    fail_session("MsgSeqNum too low, expecting " + seq_text(next_incoming_) + " but received " +
                     seq_text(seq),
                 now_ns);
}

void Session::process_in_order(const Message& message, Arrival arrival, int64_t now_ns) {
    const std::string_view type = message.msg_type();

    if (type == msg_type::SequenceReset) {  // only GapFill mode gets here
        apply_gap_fill(message, now_ns);
        return;
    }

    if (type == msg_type::Heartbeat) {
        // Nothing to do: arrival already reset the silence timer.
    } else if (type == msg_type::TestRequest) {
        send_heartbeat(now_ns, message.get(tag::TestReqID));
    } else if (type == msg_type::ResendRequest) {
        // A queued ResendRequest was already answered when it first arrived.
        if (arrival == Arrival::Live) answer_resend_request(message, now_ns);
    } else if (type == msg_type::Reject) {
        log("peer rejected our message " + std::string(message.get(tag::RefSeqNum)) + ": " +
            std::string(message.get(tag::Text)));
    } else if (type == msg_type::Logout) {
        handle_logout(message, now_ns);
    } else if (type == msg_type::Logon) {
        // Already handled when it arrived (it was queued behind a gap).
    } else if (callbacks_.on_app_message) {
        callbacks_.on_app_message(message);
    }
    // Advance after delivering: a crash in between means a re-request (at-least-once).
    // Duplicates are dropped downstream by ExecID; a lost fill can't be recovered.
    advance_incoming();
}

void Session::process_queued_messages(int64_t now_ns) {
    while (!queued_.empty() &&
           (state_ == SessionState::Active || state_ == SessionState::LogoutSent)) {
        auto first = queued_.begin();
        if (first->first < next_incoming_) {  // covered by a resend or a gap fill already
            queued_.erase(first);
            continue;
        }
        if (first->first > next_incoming_) break;  // there is still a hole before it

        // Move the bytes out first: processing the message may touch the queue.
        const std::string raw = std::move(first->second);
        queued_.erase(first);
        if (decode_message(raw, queued_message_).status == DecodeStatus::Ok) {
            process_in_order(queued_message_, Arrival::FromQueue, now_ns);
        }
    }

    if (resend_requested_ && next_incoming_ > resend_until_seq_) {
        resend_requested_ = false;
        log("gap filled; back in sequence, next expected " + seq_text(next_incoming_));
    }
}

void Session::apply_gap_fill(const Message& message, int64_t now_ns) {
    const std::optional<uint64_t> new_seq = message.get_uint(tag::NewSeqNo);
    const uint64_t seq = next_incoming_;
    if (!new_seq || *new_seq <= seq) {
        // A gap fill may only move forward. Reject it and count the message as used.
        send_reject(seq, "SequenceReset-GapFill NewSeqNo must be greater than MsgSeqNum", now_ns);
        advance_incoming();
        return;
    }
    next_incoming_ = *new_seq;
    persist_sequence_numbers();
}

void Session::apply_sequence_reset(const Message& message, int64_t now_ns) {
    const std::optional<uint64_t> new_seq = message.get_uint(tag::NewSeqNo);
    if (!new_seq || *new_seq < next_incoming_) {
        send_reject(message.get_uint(tag::MsgSeqNum).value_or(0),
                    "SequenceReset-Reset cannot lower the expected sequence number", now_ns);
        return;
    }
    log("SequenceReset-Reset: next expected " + seq_text(next_incoming_) + " -> " + seq_text(*new_seq));
    next_incoming_ = *new_seq;
    persist_sequence_numbers();
    process_queued_messages(now_ns);
}

void Session::handle_logout(const Message& message, int64_t now_ns) {
    if (state_ == SessionState::LogoutSent) {
        log("logout complete");
        state_ = SessionState::LoggedOut;
        return;
    }
    log("peer logged out: " + std::string(message.get(tag::Text)));
    send_logout("logout acknowledged", now_ns);
    state_ = SessionState::LoggedOut;
}

void Session::answer_resend_request(const Message& request, int64_t now_ns) {
    const std::optional<uint64_t> begin = request.get_uint(tag::BeginSeqNo);
    const std::optional<uint64_t> end = request.get_uint(tag::EndSeqNo);
    if (!begin || !end || *begin == 0) {
        send_reject(request.get_uint(tag::MsgSeqNum).value_or(0), "ResendRequest needs BeginSeqNo and EndSeqNo",
                    now_ns);
        return;
    }
    const uint64_t last_sent = next_outgoing_ - 1;
    const uint64_t last = (*end == 0 || *end > last_sent) ? last_sent : *end;  // 0 = through the latest
    if (*begin > last) {
        log("ResendRequest from " + seq_text(*begin) + " but our last message was " + seq_text(last_sent));
        return;
    }
    log("answering ResendRequest " + seq_text(*begin) + ".." + seq_text(last));

    // Resend stored application messages with their original numbers; each run of unstored numbers
    // (admin messages, or sent before a restart) becomes one gap fill.
    uint64_t next_to_cover = *begin;
    for (auto it = sent_messages_.lower_bound(*begin); it != sent_messages_.end() && it->first <= last; ++it) {
        if (it->first > next_to_cover) send_gap_fill(next_to_cover, it->first, now_ns);
        const SentMessage& stored = it->second;
        transmit(stored.msg_type, it->first, stored.body, now_ns, stored.sending_time_ns);
        next_to_cover = it->first + 1;
    }
    if (next_to_cover <= last) send_gap_fill(next_to_cover, last + 1, now_ns);
}

bool Session::has_expected_comp_ids(const Message& message) const {
    return message.get(tag::SenderCompID) == config_.target_comp_id &&
           message.get(tag::TargetCompID) == config_.sender_comp_id;
}

void Session::advance_incoming() {
    ++next_incoming_;
    persist_sequence_numbers();
}

uint64_t Session::send_new_message(std::string_view type, std::string_view body, int64_t now_ns,
                                   bool is_application) {
    const uint64_t seq = next_outgoing_++;
    // Persist before sending so a crash can never reuse a number the peer has already seen.
    persist_sequence_numbers();
    if (is_application) store_sent_message(seq, type, body, now_ns);

    if (is_application && state_ != SessionState::Active) {
        log("not logged on: message " + seq_text(seq) + " stored, it goes out with the resend after the next logon");
        return seq;
    }
    if (is_application && drop_next_outgoing_app_message_) {
        drop_next_outgoing_app_message_ = false;
        log("TEST HOOK: not sending message " + seq_text(seq) + " (MsgType " + std::string(type) +
            "); the other side will see a gap");
        return seq;
    }
    transmit(type, seq, body, now_ns, std::nullopt);
    return seq;
}

void Session::transmit(std::string_view type, uint64_t seq, std::string_view body, int64_t now_ns,
                       std::optional<int64_t> orig_sending_time_ns) {
    // PossDupFlag and OrigSendingTime only on resends and gap fills.
    builder_.start(type);
    builder_.add(tag::SenderCompID, config_.sender_comp_id);
    builder_.add(tag::TargetCompID, config_.target_comp_id);
    builder_.add_int(tag::MsgSeqNum, static_cast<int64_t>(seq));
    if (orig_sending_time_ns) builder_.add_char(tag::PossDupFlag, 'Y');
    builder_.add_timestamp(tag::SendingTime, now_ns);
    if (orig_sending_time_ns) builder_.add_timestamp(tag::OrigSendingTime, *orig_sending_time_ns);
    builder_.add_encoded_fields(body);

    const std::string_view bytes = builder_.finish();
    last_sent_ns_ = now_ns;
    if (callbacks_.on_wire_message) callbacks_.on_wire_message(Direction::Outbound, bytes);
    callbacks_.send_bytes(bytes);
}

void Session::send_logon(int64_t now_ns, bool reset_seq_num) {
    admin_body_.clear();
    append_int_field(admin_body_, tag::EncryptMethod, 0);  // 0 = none
    append_int_field(admin_body_, tag::HeartBtInt, heartbeat_interval_seconds_);
    if (reset_seq_num) append_char_field(admin_body_, tag::ResetSeqNumFlag, 'Y');
    send_new_message(msg_type::Logon, admin_body_, now_ns, /*is_application=*/false);
}

void Session::send_heartbeat(int64_t now_ns, std::string_view test_request_id) {
    admin_body_.clear();
    // A Heartbeat that answers a TestRequest must echo its TestReqID.
    if (!test_request_id.empty()) append_field(admin_body_, tag::TestReqID, test_request_id);
    send_new_message(msg_type::Heartbeat, admin_body_, now_ns, /*is_application=*/false);
}

void Session::send_test_request(int64_t now_ns) {
    admin_body_.clear();
    append_field(admin_body_, tag::TestReqID, "TEST" + std::to_string(++test_request_counter_));
    send_new_message(msg_type::TestRequest, admin_body_, now_ns, /*is_application=*/false);
    test_request_pending_ = true;
    test_request_sent_ns_ = now_ns;
}

void Session::send_resend_request(int64_t now_ns) {
    admin_body_.clear();
    append_int_field(admin_body_, tag::BeginSeqNo, static_cast<int64_t>(next_incoming_));
    append_int_field(admin_body_, tag::EndSeqNo, 0);  // 0 = everything after BeginSeqNo
    send_new_message(msg_type::ResendRequest, admin_body_, now_ns, /*is_application=*/false);
    resend_requested_ = true;
    resend_request_sent_ns_ = now_ns;
}

void Session::send_gap_fill(uint64_t seq, uint64_t new_seq, int64_t now_ns) {
    admin_body_.clear();
    append_char_field(admin_body_, tag::GapFillFlag, 'Y');
    append_int_field(admin_body_, tag::NewSeqNo, static_cast<int64_t>(new_seq));
    // Gap fill takes the first skipped number and PossDupFlag=Y. Admin send times aren't kept,
    // so OrigSendingTime = now.
    transmit(msg_type::SequenceReset, seq, admin_body_, now_ns, now_ns);
}

void Session::send_logout(std::string_view text, int64_t now_ns) {
    admin_body_.clear();
    if (!text.empty()) append_field(admin_body_, tag::Text, text);
    send_new_message(msg_type::Logout, admin_body_, now_ns, /*is_application=*/false);
}

void Session::send_reject(uint64_t ref_seq, std::string_view text, int64_t now_ns) {
    log("rejecting message " + seq_text(ref_seq) + ": " + std::string(text));
    admin_body_.clear();
    append_int_field(admin_body_, tag::RefSeqNum, static_cast<int64_t>(ref_seq));
    append_field(admin_body_, tag::Text, text);
    send_new_message(msg_type::Reject, admin_body_, now_ns, /*is_application=*/false);
}

void Session::store_sent_message(uint64_t seq, std::string_view type, std::string_view body, int64_t now_ns) {
    // In memory only: after a restart, older numbers are answered with gap fills.
    sent_messages_.emplace(seq, SentMessage{std::string(type), std::string(body), now_ns});
    if (sent_messages_.size() > kMaxStoredMessages) sent_messages_.erase(sent_messages_.begin());
}

void Session::fail_session(const std::string& reason, int64_t now_ns) {
    log("session error: " + reason);
    send_logout(reason, now_ns);  // tell the other side why before hanging up
    state_ = SessionState::LoggedOut;
}

void Session::check_heartbeat_timers(int64_t now_ns) {
    const int64_t interval_ns = static_cast<int64_t>(heartbeat_interval_seconds_) * kNanosPerSecond;
    // 20% grace so a slightly late heartbeat doesn't trigger a TestRequest.
    const int64_t silence_limit_ns = interval_ns + interval_ns / 5;

    if (now_ns - last_sent_ns_ >= interval_ns) send_heartbeat(now_ns, {});

    if (now_ns - last_received_ns_ >= silence_limit_ns) {
        if (!test_request_pending_) {
            log("nothing received for 1.2 x HeartBtInt; sending TestRequest");
            send_test_request(now_ns);
        } else if (now_ns - test_request_sent_ns_ >= silence_limit_ns) {
            fail_session("no response to TestRequest; connection presumed dead", now_ns);
            return;
        }
    }

    // Re-request if a resend never completes; overlapping duplicates carry PossDupFlag=Y and are dropped.
    if (resend_requested_ && now_ns - resend_request_sent_ns_ >= interval_ns) {
        log("resend not finished after HeartBtInt; asking again from " + seq_text(next_incoming_));
        send_resend_request(now_ns);
    }
}

void Session::reset_sequence_numbers() {
    next_outgoing_ = 1;
    next_incoming_ = 1;
    sent_messages_.clear();
    queued_.clear();
    resend_requested_ = false;
    resend_until_seq_ = 0;
    persist_sequence_numbers();
}

void Session::persist_sequence_numbers() {
    if (!store_.save(next_outgoing_, next_incoming_)) log("failed to write sequence file " + store_.path());
}

void Session::log(const std::string& text) const {
    if (callbacks_.on_log) callbacks_.on_log(text);
}

}  // namespace mt::fix
