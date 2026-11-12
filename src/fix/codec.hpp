#pragma once
// FIX tag=value codec: "tag=value<SOH>" fields framed by 8=FIX.4.4, 9=<BodyLength> and 10=<CheckSum>.
// The decoder handles partial TCP reads. Buffers are reused and decoded values are string_views,
// so nothing allocates per message after warm-up.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fix/fix_tags.hpp"

namespace mt::fix {

inline constexpr char kSoh = '\x01';
inline constexpr std::string_view kBeginString = "FIX.4.4";
// Larger BodyLength values are treated as corrupt.
inline constexpr size_t kMaxBodyLength = 64 * 1024;

// Append one "tag=value<SOH>" field to `out`. Values must not contain SOH.
void append_field(std::string& out, int tag, std::string_view value);
void append_int_field(std::string& out, int tag, int64_t value);
void append_char_field(std::string& out, int tag, char value);
// UTCTimestamp with milliseconds: "20261111-20:45:00.123".
void append_timestamp_field(std::string& out, int tag, int64_t wall_ns);
void append_utc_timestamp(std::string& out, int64_t wall_ns);

// Sum of the bytes mod 256: the value of CheckSum(10).
int compute_checksum(std::string_view bytes);

// Builds one message: start(msg_type), add fields, then finish().
// Adds 8=, 9= and 10= itself; MsgType(35) is always the first body field.
class MessageBuilder {
public:
    void start(std::string_view msg_type);
    void add(int tag, std::string_view value) { append_field(body_, tag, value); }
    void add_int(int tag, int64_t value) { append_int_field(body_, tag, value); }
    void add_char(int tag, char value) { append_char_field(body_, tag, value); }
    void add_timestamp(int tag, int64_t wall_ns) { append_timestamp_field(body_, tag, wall_ns); }
    // Already-encoded "tag=value<SOH>..." fields (an application body).
    void add_encoded_fields(std::string_view fields) { body_.append(fields); }

    // Adds BeginString, BodyLength and CheckSum. The view is valid until the next start().
    std::string_view finish();

private:
    std::string body_;     // 35=... up to the last body field
    std::string message_;  // the finished message
};

enum class DecodeStatus : uint8_t {
    Ok,              // one complete, valid message
    NeedMoreBytes,   // the buffer holds only the start of a message: read more
    Garbled,         // bytes that are not a FIX message (or a malformed field)
    BadBeginString,  // 8= is not FIX.4.4
    BadBodyLength,   // 9= is missing or not a number, or 10= is not where it says
    BadChecksum,     // framing is fine but the bytes were corrupted
};

const char* to_string(DecodeStatus status);

struct Field {
    int tag = 0;
    std::string_view value;
};

// Decoded message: (tag, value) views into the receive buffer, in wire order.
// Valid only while that buffer is unchanged; copy anything you need to keep.
class Message {
public:
    void clear() {
        fields_.clear();
        raw_ = {};
    }
    void add_field(int tag, std::string_view value) { fields_.push_back(Field{tag, value}); }
    void set_raw(std::string_view raw) { raw_ = raw; }

    // First value for `tag`, or empty if absent (FIX forbids empty values). Linear scan:
    // ~20 fields in a small vector beats hashing and doesn't allocate.
    std::string_view get(int tag) const {
        for (const Field& field : fields_) {
            if (field.tag == tag) return field.value;
        }
        return {};
    }
    bool has(int tag) const { return !get(tag).empty(); }
    bool get_flag(int tag) const { return get(tag) == "Y"; }
    // Unsigned integer value; nullopt if absent or not all digits.
    std::optional<uint64_t> get_uint(int tag) const;
    char get_char(int tag) const {
        const std::string_view value = get(tag);
        return value.empty() ? '\0' : value[0];
    }

    std::string_view msg_type() const { return get(tag::MsgType); }
    std::string_view raw() const { return raw_; }
    const std::vector<Field>& fields() const { return fields_; }

private:
    std::vector<Field> fields_;
    std::string_view raw_;
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::NeedMoreBytes;
    // Bytes to drop from the front: message length (Ok), bad bytes (error), 0 (NeedMoreBytes).
    size_t consumed = 0;
};

// Decodes the first message in `buffer`. On Ok, `out` holds views into `buffer`.
DecodeResult decode_message(std::string_view buffer, Message& out);

// Accumulates socket bytes: append() after each read, then next() until NeedMoreBytes.
// Returned messages point into the internal buffer and are valid until the next append() or clear().
class StreamDecoder {
public:
    void append(std::string_view bytes);
    DecodeResult next(Message& out);
    void clear();
    size_t buffered_bytes() const { return buffer_.size() - read_offset_; }

private:
    std::string buffer_;
    size_t read_offset_ = 0;  // bytes at the front already handed out or skipped
};

// For logs: the message with SOH shown as '|'.
std::string to_printable(std::string_view raw);

}  // namespace mt::fix
