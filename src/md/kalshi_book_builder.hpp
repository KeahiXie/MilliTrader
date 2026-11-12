#pragma once
// KalshiBookBuilder: turns Kalshi orderbook_snapshot/orderbook_delta messages into
// FeedEvents and recovers from sequence gaps. No sockets or threads, so gaps can be forced in tests.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/events.hpp"
#include "core/instruments.hpp"
#include "md/kalshi_rest.hpp"
#include "md/kalshi_ws_protocol.hpp"
#include "md/market_order_book.hpp"

namespace mt {

struct KalshiSnapshotMessage {
    int64_t sid = 0;
    int64_t seq = 0;
    std::string market_ticker;
    BookSnapshot book;
};

struct KalshiDeltaMessage {
    int64_t sid = 0;
    int64_t seq = 0;
    KalshiBookDelta delta;
};

// seq is per subscription (sid), so a gap means some book on the sid is wrong. Recovery: mark all
// stale, request snapshots, keep buffering deltas, then replay only deltas newer than each snapshot.
class KalshiBookBuilder {
public:
    using EventSink = std::function<void(const FeedEvent&)>;
    using SnapshotRequester = std::function<void(const std::vector<std::string>& market_tickers)>;

    KalshiBookBuilder(const InstrumentTable& instruments, EventSink sink, SnapshotRequester request_snapshots);

    // Markets on this subscription; their instruments must already be in the InstrumentTable.
    void add_market(const std::string& ticker);
    void remove_market(const std::string& ticker);

    void on_snapshot(const KalshiSnapshotMessage& message, int64_t receive_ns);
    void on_delta(const KalshiDeltaMessage& message, int64_t receive_ns);

    // After a reconnect the sid restarts: forget the sequence and wait for new snapshots.
    void reset_for_new_subscription();

    // Socket receive time of the current frame, copied into its events for decode latency.
    void set_receive_mono_ns(int64_t mono_ns) { current_receive_mono_ns_ = mono_ns; }

    bool recovering() const { return recovering_; }
    bool needs_reconnect() const { return failed_rounds_ >= 3; }

    int64_t gaps_detected() const { return gaps_detected_; }
    int64_t recoveries_completed() const { return recoveries_completed_; }
    int64_t duplicates_ignored() const { return duplicates_ignored_; }
    int64_t deltas_replayed() const { return deltas_replayed_; }

    // For tests and invariant checks.
    const MarketOrderBook* book(const std::string& ticker) const;

private:
    void start_recovery(int64_t receive_ns, const char* reason);
    void finish_recovery(int64_t receive_ns);
    void apply_snapshot(const KalshiSnapshotMessage& message, int64_t receive_ns);
    bool apply_delta(const KalshiDeltaMessage& message, int64_t receive_ns);
    void emit(FeedEventType type, InstrumentId instrument, Side side, Price price, Qty qty, uint64_t seq,
              int64_t receive_ns, int64_t exchange_ns = 0);
    InstrumentId instrument_for(const std::string& ticker) const;

    const InstrumentTable& instruments_;
    EventSink sink_;
    SnapshotRequester request_snapshots_;

    std::set<std::string> markets_;
    int64_t last_seq_ = 0;  // 0 = nothing seen yet on this subscription

    // Recovery state
    bool recovering_ = false;
    std::set<std::string> waiting_for_snapshot_;
    std::map<std::string, int64_t> snapshot_seq_;  // market -> seq of its fresh snapshot
    std::vector<KalshiDeltaMessage> buffered_;
    std::set<int64_t> seqs_seen_while_recovering_;
    int failed_rounds_ = 0;

    // Own copy of each book: a delta that would make a level negative proves we're out of sync.
    std::map<std::string, std::unique_ptr<MarketOrderBook>> books_;

    int64_t current_receive_mono_ns_ = 0;
    int64_t gaps_detected_ = 0;
    int64_t recoveries_completed_ = 0;
    int64_t duplicates_ignored_ = 0;
    int64_t deltas_replayed_ = 0;
};

}  // namespace mt
