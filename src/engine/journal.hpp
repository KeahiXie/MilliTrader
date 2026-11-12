#pragma once
// Journal: append-only binary log of every engine input and decision, written by a background thread.
// The engine only copies records into an SPSC ring; it never formats, calls write() or waits on disk.
// Format: "MTJRNL02", 7 record sizes, logic version, build id; then u16 type | u16 size | payload.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include "core/events.hpp"
#include "core/instruments.hpp"
#include "core/spsc_queue.hpp"
#include "engine/trade_engine.hpp"

namespace mt {

enum class JournalRecordType : uint16_t {
    Config = 1,
    Instrument = 2,
    Feed = 3,
    Gateway = 4,
    Control = 5,
    OrderOut = 6,
    RiskReject = 7,
};

struct RiskRejectRecord {
    OrderRequest order;
    RiskCheckResult reason = RiskCheckResult::Allowed;
    int64_t engine_time_ns = 0;
};

struct OrderOutRecord {
    OrderRequest order;
    int64_t engine_time_ns = 0;
};

using JournalRecord = std::variant<EngineConfig, InstrumentInfo, FeedEvent, GatewayEvent, ControlEvent, OrderOutRecord,
                                   RiskRejectRecord>;

JournalRecordType record_type(const JournalRecord& record);

class JournalWriter {
public:
    // instruments (optional): the writer thread journals each new definition in the table.
    JournalWriter(std::string path, const InstrumentTable* instruments, size_t queue_capacity = 1 << 18);
    ~JournalWriter();

    bool start();
    void stop();  // drains the queue, flushes and closes

    // Engine thread only.
    void push(const JournalRecord& record);

    int64_t records_written() const { return records_written_.load(std::memory_order_relaxed); }
    const std::string& path() const { return path_; }

private:
    void run();
    void write_record(const JournalRecord& record);

    std::string path_;
    const InstrumentTable* instruments_;
    SpscQueue<JournalRecord> queue_;
    std::FILE* file_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> records_written_{0};
    InstrumentId instruments_written_ = 0;
};

class JournalReader {
public:
    explicit JournalReader(const std::string& path);
    ~JournalReader();
    bool ok() const { return file_ != nullptr && error_.empty(); }
    const std::string& error() const { return error_; }
    std::optional<JournalRecord> next();
    // File ended mid-record (the writer was killed).
    bool truncated_tail() const { return truncated_tail_; }
    uint32_t engine_logic_version() const { return engine_logic_version_; }
    const std::string& build_id() const { return build_id_; }

private:
    std::FILE* file_ = nullptr;
    std::string error_;
    bool truncated_tail_ = false;
    uint32_t engine_logic_version_ = 0;
    std::string build_id_;
};

// This binary's git describe ("a5d9f7d-dirty"), set by CMake.
const char* build_id();

}  // namespace mt
