#include "engine/journal.hpp"

#include <chrono>
#include <cstring>
#include <type_traits>

#include "core/clock.hpp"
#include "core/thread_utils.hpp"

namespace mt {

namespace {

constexpr char kMagic[8] = {'M', 'T', 'J', 'R', 'N', 'L', '0', '2'};
constexpr char kMagicV1[8] = {'M', 'T', 'J', 'R', 'N', 'L', '0', '1'};  // before the version stamp
constexpr size_t kBuildIdSize = 40;

static_assert(std::is_trivially_copyable_v<EngineConfig>);
static_assert(std::is_trivially_copyable_v<InstrumentInfo>);
static_assert(std::is_trivially_copyable_v<RiskRejectRecord>);
static_assert(std::is_trivially_copyable_v<OrderOutRecord>);

// Record sizes go in the header so a build with a different struct layout refuses the file.
const uint32_t kRecordSizes[7] = {
    sizeof(EngineConfig), sizeof(InstrumentInfo), sizeof(FeedEvent),      sizeof(GatewayEvent),
    sizeof(ControlEvent), sizeof(OrderOutRecord), sizeof(RiskRejectRecord),
};

enum class ReadResult { Ok, Corrupt, TruncatedAtEnd };

template <typename T>
ReadResult read_payload(std::FILE* file, uint16_t size, JournalRecord* out) {
    if (size != sizeof(T)) return ReadResult::Corrupt;
    T value;
    if (std::fread(&value, sizeof(T), 1, file) != 1) {
        // half a record at EOF means the writer was killed: end of journal, not corruption
        return std::feof(file) ? ReadResult::TruncatedAtEnd : ReadResult::Corrupt;
    }
    *out = value;
    return ReadResult::Ok;
}

}  // namespace

const char* build_id() {
#ifdef MT_BUILD_ID
    return MT_BUILD_ID;
#else
    return "unknown";
#endif
}

JournalRecordType record_type(const JournalRecord& record) {
    return static_cast<JournalRecordType>(record.index() + 1);  // variant order == enum order
}

JournalWriter::JournalWriter(std::string path, const InstrumentTable* instruments, size_t queue_capacity)
    : path_(std::move(path)), instruments_(instruments), queue_(queue_capacity) {}

JournalWriter::~JournalWriter() { stop(); }

bool JournalWriter::start() {
    file_ = std::fopen(path_.c_str(), "wb");
    if (file_ == nullptr) return false;
    std::fwrite(kMagic, 1, sizeof(kMagic), file_);
    std::fwrite(kRecordSizes, sizeof(kRecordSizes), 1, file_);
    const uint32_t version = kEngineLogicVersion;
    std::fwrite(&version, sizeof(version), 1, file_);
    char build[kBuildIdSize] = {};
    copy_cstring(build, sizeof(build), mt::build_id());
    std::fwrite(build, 1, sizeof(build), file_);
    running_ = true;
    thread_ = start_thread("mt-journal", -1, [this] { run(); });
    return true;
}

void JournalWriter::stop() {
    if (!running_.exchange(false)) return;
    thread_.join();
    std::fclose(file_);
    file_ = nullptr;
}

void JournalWriter::push(const JournalRecord& record) {
    while (!queue_.try_push(record)) cpu_relax();  // never drop: a journal with a hole can't be replayed
}

void JournalWriter::write_record(const JournalRecord& record) {
    const uint16_t type = static_cast<uint16_t>(record_type(record));
    std::visit(
        [&](const auto& value) {
            const uint16_t size = static_cast<uint16_t>(sizeof(value));
            std::fwrite(&type, sizeof(type), 1, file_);
            std::fwrite(&size, sizeof(size), 1, file_);
            std::fwrite(&value, sizeof(value), 1, file_);
        },
        record);
    records_written_.fetch_add(1, std::memory_order_relaxed);
}

void JournalWriter::run() {
    JournalRecord record;
    int64_t last_flush = monotonic_ns();
    auto write_new_instruments = [this] {
        if (instruments_ == nullptr) return;
        const InstrumentId count = instruments_->size();
        for (; instruments_written_ < count; ++instruments_written_) {
            write_record(*instruments_->find(instruments_written_));
        }
    };
    while (true) {
        bool any = false;
        while (queue_.try_pop(record)) {
            // new instruments first, so definitions come before their first use
            write_new_instruments();
            write_record(record);
            any = true;
        }
        const int64_t now = monotonic_ns();
        if (now - last_flush > 100 * kNanosPerMilli) {
            write_new_instruments();
            std::fflush(file_);
            last_flush = now;
        }
        if (!running_ && !any) break;
        if (!any) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    write_new_instruments();
    std::fflush(file_);
}

JournalReader::JournalReader(const std::string& path) {
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        error_ = "cannot open " + path;
        return;
    }
    char magic[8];
    uint32_t sizes[7];
    if (std::fread(magic, 1, 8, file_) != 8) {
        error_ = "not a MilliTrader journal";
        return;
    }
    const bool v2 = std::memcmp(magic, kMagic, 8) == 0;
    if (!v2 && std::memcmp(magic, kMagicV1, 8) != 0) {
        error_ = "not a MilliTrader journal";
        return;
    }
    if (std::fread(sizes, sizeof(sizes), 1, file_) != 1 || std::memcmp(sizes, kRecordSizes, sizeof(sizes)) != 0) {
        error_ = "journal was written by a build with different record layouts";
        return;
    }
    engine_logic_version_ = 0;  // MTJRNL01: written before the stamp existed, version unknown
    if (v2) {
        char build[kBuildIdSize] = {};
        if (std::fread(&engine_logic_version_, sizeof(engine_logic_version_), 1, file_) != 1 ||
            std::fread(build, 1, sizeof(build), file_) != sizeof(build)) {
            error_ = "truncated journal header";
            return;
        }
        build[kBuildIdSize - 1] = '\0';
        build_id_ = build;
    }
}

JournalReader::~JournalReader() {
    if (file_ != nullptr) std::fclose(file_);
}

std::optional<JournalRecord> JournalReader::next() {
    if (!ok()) return std::nullopt;
    uint16_t type = 0, size = 0;
    if (std::fread(&type, sizeof(type), 1, file_) != 1) return std::nullopt;  // clean end of file
    if (std::fread(&size, sizeof(size), 1, file_) != 1) {
        truncated_tail_ = true;  // the header itself was cut off
        return std::nullopt;
    }
    JournalRecord record;
    ReadResult result = ReadResult::Corrupt;
    switch (static_cast<JournalRecordType>(type)) {
        case JournalRecordType::Config: result = read_payload<EngineConfig>(file_, size, &record); break;
        case JournalRecordType::Instrument: result = read_payload<InstrumentInfo>(file_, size, &record); break;
        case JournalRecordType::Feed: result = read_payload<FeedEvent>(file_, size, &record); break;
        case JournalRecordType::Gateway: result = read_payload<GatewayEvent>(file_, size, &record); break;
        case JournalRecordType::Control: result = read_payload<ControlEvent>(file_, size, &record); break;
        case JournalRecordType::OrderOut: result = read_payload<OrderOutRecord>(file_, size, &record); break;
        case JournalRecordType::RiskReject: result = read_payload<RiskRejectRecord>(file_, size, &record); break;
    }
    if (result == ReadResult::TruncatedAtEnd) {
        truncated_tail_ = true;
        return std::nullopt;
    }
    if (result == ReadResult::Corrupt) {
        error_ = "corrupt record (type " + std::to_string(type) + ")";
        return std::nullopt;
    }
    return record;
}

}  // namespace mt
