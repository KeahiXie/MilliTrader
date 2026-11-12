#include "fix/codec.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdio>
#include <ctime>

namespace mt::fix {

namespace {

// Start of every message; searched for to resync after garbage.
constexpr std::string_view kBeginPrefix = "8=FIX";
constexpr std::string_view kBeginStringField = "8=FIX.4.4\x01";
constexpr size_t kTrailerLength = 7;     // "10=" + 3 digits + SOH
constexpr size_t kMaxLengthFieldBytes = 16;  // "9=" + digits + SOH, generously

bool is_digit(char c) { return c >= '0' && c <= '9'; }

void append_unsigned(std::string& out, uint64_t value) {
    char digits[24];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    out.append(digits, result.ptr);
}

// Offset of the next possible message start at or after `from` (>= 1 after an error, so we always
// make progress). With no "8=FIX" yet, keep any tail that could become one on the next read.
size_t resync_offset(std::string_view buffer, size_t from) {
    const size_t next = buffer.find(kBeginPrefix, from);
    if (next != std::string_view::npos) return next;
    const size_t longest_tail = std::min(kBeginPrefix.size() - 1, buffer.size() - from);
    for (size_t keep = longest_tail; keep > 0; --keep) {
        if (buffer.substr(buffer.size() - keep) == kBeginPrefix.substr(0, keep)) {
            return buffer.size() - keep;
        }
    }
    return buffer.size();
}

enum class ScanStatus : uint8_t { Complete, Incomplete, Malformed };

struct ScannedField {
    ScanStatus status = ScanStatus::Malformed;
    int tag = 0;
    std::string_view value;
    size_t next = 0;  // offset just past the field's SOH
};

// Reads one "tag=value<SOH>" field starting at `position`.
ScannedField scan_field(std::string_view buffer, size_t position) {
    ScannedField field;
    size_t i = position;
    int digits = 0;
    while (i < buffer.size() && is_digit(buffer[i])) {
        if (++digits > 9) return field;  // no real tag has 10 digits
        field.tag = field.tag * 10 + (buffer[i] - '0');
        ++i;
    }
    if (i == buffer.size()) {
        field.status = ScanStatus::Incomplete;
        return field;
    }
    if (digits == 0 || buffer[i] != '=') return field;

    const size_t value_start = i + 1;
    const size_t end = buffer.find(kSoh, value_start);
    if (end == std::string_view::npos) {
        field.status = ScanStatus::Incomplete;
        return field;
    }
    if (end == value_start) return field;  // "58=<SOH>": empty values are not allowed

    field.status = ScanStatus::Complete;
    field.value = buffer.substr(value_start, end - value_start);
    field.next = end + 1;
    return field;
}

std::optional<uint64_t> parse_unsigned(std::string_view text) {
    if (text.empty() || text.size() > 19) return std::nullopt;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

DecodeResult need_more_bytes() { return {DecodeStatus::NeedMoreBytes, 0}; }

}  // namespace

void append_field(std::string& out, int tag, std::string_view value) {
    assert(value.find(kSoh) == std::string_view::npos && "FIX values cannot contain SOH");
    append_unsigned(out, static_cast<uint64_t>(tag));
    out.push_back('=');
    out.append(value);
    out.push_back(kSoh);
}

void append_int_field(std::string& out, int tag, int64_t value) {
    append_unsigned(out, static_cast<uint64_t>(tag));
    out.push_back('=');
    char digits[24];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    out.append(digits, result.ptr);
    out.push_back(kSoh);
}

void append_char_field(std::string& out, int tag, char value) {
    append_field(out, tag, std::string_view(&value, 1));
}

void append_utc_timestamp(std::string& out, int64_t wall_ns) {
    const time_t seconds = static_cast<time_t>(wall_ns / 1'000'000'000);
    const int millis = static_cast<int>((wall_ns / 1'000'000) % 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char text[64];
    const int length = std::snprintf(text, sizeof(text), "%04d%02d%02d-%02d:%02d:%02d.%03d",
                                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                                     utc.tm_min, utc.tm_sec, millis);
    out.append(text, static_cast<size_t>(length));
}

void append_timestamp_field(std::string& out, int tag, int64_t wall_ns) {
    append_unsigned(out, static_cast<uint64_t>(tag));
    out.push_back('=');
    append_utc_timestamp(out, wall_ns);
    out.push_back(kSoh);
}

int compute_checksum(std::string_view bytes) {
    unsigned sum = 0;
    for (const char c : bytes) sum += static_cast<unsigned char>(c);
    return static_cast<int>(sum % 256);
}

void MessageBuilder::start(std::string_view msg_type) {
    body_.clear();  // keeps capacity: no allocation after warm-up
    append_field(body_, tag::MsgType, msg_type);
}

std::string_view MessageBuilder::finish() {
    message_.clear();
    message_.append(kBeginStringField);
    message_.append("9=");
    append_unsigned(message_, body_.size());
    message_.push_back(kSoh);
    message_.append(body_);

    const int checksum = compute_checksum(message_);
    const char trailer[kTrailerLength] = {
        '1', '0', '=', static_cast<char>('0' + checksum / 100),
        static_cast<char>('0' + (checksum / 10) % 10), static_cast<char>('0' + checksum % 10), kSoh};
    message_.append(trailer, kTrailerLength);
    return message_;
}

const char* to_string(DecodeStatus status) {
    switch (status) {
        case DecodeStatus::Ok: return "ok";
        case DecodeStatus::NeedMoreBytes: return "need more bytes";
        case DecodeStatus::Garbled: return "garbled";
        case DecodeStatus::BadBeginString: return "bad BeginString";
        case DecodeStatus::BadBodyLength: return "bad BodyLength";
        case DecodeStatus::BadChecksum: return "bad CheckSum";
    }
    return "?";
}

std::optional<uint64_t> Message::get_uint(int tag) const { return parse_unsigned(get(tag)); }

DecodeResult decode_message(std::string_view buffer, Message& out) {
    out.clear();

    // 1. Anything before "8=FIX" is garbage.
    if (buffer.substr(0, kBeginPrefix.size()) != kBeginPrefix) {
        const size_t garbage = resync_offset(buffer, 0);
        if (garbage == 0) return need_more_bytes();  // just the first bytes of "8=FIX"
        return {DecodeStatus::Garbled, garbage};
    }

    // 2. BeginString must be exactly FIX.4.4.
    if (buffer.size() < kBeginStringField.size()) {
        if (kBeginStringField.substr(0, buffer.size()) == buffer) return need_more_bytes();
        return {DecodeStatus::BadBeginString, resync_offset(buffer, 1)};
    }
    if (buffer.substr(0, kBeginStringField.size()) != kBeginStringField) {
        return {DecodeStatus::BadBeginString, resync_offset(buffer, 1)};
    }

    // 3. BodyLength tells us where the message ends.
    const ScannedField length_field = scan_field(buffer, kBeginStringField.size());
    if (length_field.status == ScanStatus::Incomplete) {
        if (buffer.size() - kBeginStringField.size() < kMaxLengthFieldBytes) return need_more_bytes();
        return {DecodeStatus::BadBodyLength, resync_offset(buffer, 1)};
    }
    const std::optional<uint64_t> body_length =
        length_field.status == ScanStatus::Complete && length_field.tag == tag::BodyLength
            ? parse_unsigned(length_field.value)
            : std::nullopt;
    if (!body_length || *body_length == 0 || *body_length > kMaxBodyLength) {
        return {DecodeStatus::BadBodyLength, resync_offset(buffer, 1)};
    }

    const size_t body_start = length_field.next;
    const size_t body_end = body_start + *body_length;
    const size_t message_end = body_end + kTrailerLength;
    if (buffer.size() < message_end) return need_more_bytes();

    // 4. "10=NNN<SOH>" must sit exactly where BodyLength says. If not, no boundary can be trusted,
    //    so skip ahead to the next "8=FIX".
    const std::string_view trailer = buffer.substr(body_end, kTrailerLength);
    const bool trailer_ok = buffer[body_end - 1] == kSoh && trailer.substr(0, 3) == "10=" &&
                            is_digit(trailer[3]) && is_digit(trailer[4]) && is_digit(trailer[5]) &&
                            trailer[6] == kSoh;
    if (!trailer_ok) return {DecodeStatus::BadBodyLength, resync_offset(buffer, 1)};

    // 5. CheckSum. The boundary is known, so only this one message is dropped.
    const int received_checksum = (trailer[3] - '0') * 100 + (trailer[4] - '0') * 10 + (trailer[5] - '0');
    if (compute_checksum(buffer.substr(0, body_end)) != received_checksum) {
        return {DecodeStatus::BadChecksum, message_end};
    }

    // 6. Split into fields. MsgType(35) must be the first field of the body.
    const std::string_view message = buffer.substr(0, message_end);
    size_t position = 0;
    while (position < message.size()) {
        const ScannedField field = scan_field(message, position);
        if (field.status != ScanStatus::Complete) {
            out.clear();
            return {DecodeStatus::Garbled, message_end};
        }
        out.add_field(field.tag, field.value);
        position = field.next;
    }
    if (out.fields().size() < 4 || out.fields()[2].tag != tag::MsgType) {
        out.clear();
        return {DecodeStatus::Garbled, message_end};
    }

    out.set_raw(message);
    return {DecodeStatus::Ok, message_end};
}

void StreamDecoder::append(std::string_view bytes) {
    // Drop consumed bytes; what's left is at most one partial message, so this is cheap.
    if (read_offset_ > 0) {
        buffer_.erase(0, read_offset_);
        read_offset_ = 0;
    }
    buffer_.append(bytes);
}

DecodeResult StreamDecoder::next(Message& out) {
    const std::string_view pending(buffer_.data() + read_offset_, buffer_.size() - read_offset_);
    const DecodeResult result = decode_message(pending, out);
    read_offset_ += result.consumed;
    return result;
}

void StreamDecoder::clear() {
    buffer_.clear();
    read_offset_ = 0;
}

std::string to_printable(std::string_view raw) {
    std::string text(raw);
    std::replace(text.begin(), text.end(), kSoh, '|');
    return text;
}

bool is_admin_message(std::string_view type) {
    return type == msg_type::Heartbeat || type == msg_type::TestRequest ||
           type == msg_type::ResendRequest || type == msg_type::Reject ||
           type == msg_type::SequenceReset || type == msg_type::Logout || type == msg_type::Logon;
}

}  // namespace mt::fix
