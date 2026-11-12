#include "md/kalshi_book_builder.hpp"

#include <algorithm>
#include <vector>

#include "core/clock.hpp"

namespace mt {

KalshiBookBuilder::KalshiBookBuilder(const InstrumentTable& instruments, EventSink sink,
                                     SnapshotRequester request_snapshots)
    : instruments_(instruments), sink_(std::move(sink)), request_snapshots_(std::move(request_snapshots)) {}

void KalshiBookBuilder::add_market(const std::string& ticker) {
    markets_.insert(ticker);
    if (!books_.count(ticker)) books_[ticker] = std::make_unique<MarketOrderBook>();
    // A market added mid-stream is not consistent until its first snapshot.
    if (recovering_) waiting_for_snapshot_.insert(ticker);
}

void KalshiBookBuilder::remove_market(const std::string& ticker) {
    markets_.erase(ticker);
    books_.erase(ticker);
    waiting_for_snapshot_.erase(ticker);
    if (recovering_ && waiting_for_snapshot_.empty()) finish_recovery(0);
}

const MarketOrderBook* KalshiBookBuilder::book(const std::string& ticker) const {
    const auto it = books_.find(ticker);
    return it == books_.end() ? nullptr : it->second.get();
}

InstrumentId KalshiBookBuilder::instrument_for(const std::string& ticker) const {
    return instruments_.find_id(Venue::Kalshi, ticker);
}

void KalshiBookBuilder::emit(FeedEventType type, InstrumentId instrument, Side side, Price price, Qty qty,
                             uint64_t seq, int64_t receive_ns, int64_t exchange_ns) {
    FeedEvent event;
    event.type = type;
    event.venue = Venue::Kalshi;
    event.instrument = instrument;
    event.side = side;
    event.price = price;
    event.qty = qty;
    event.sequence = seq;
    event.receive_ns = receive_ns;
    event.exchange_ns = exchange_ns;
    event.receive_mono_ns = current_receive_mono_ns_;
    event.enqueue_mono_ns = monotonic_ns();
    sink_(event);
}

void KalshiBookBuilder::reset_for_new_subscription() {
    last_seq_ = 0;
    recovering_ = false;
    waiting_for_snapshot_.clear();
    snapshot_seq_.clear();
    buffered_.clear();
    seqs_seen_while_recovering_.clear();
    for (const std::string& ticker : markets_) {
        const InstrumentId id = instrument_for(ticker);
        if (id != kNoInstrument) emit(FeedEventType::BookStale, id, Side::Buy, Price{}, Qty{}, 0, wall_clock_ns());
    }
}

void KalshiBookBuilder::start_recovery(int64_t receive_ns, const char* /*reason*/) {
    if (!recovering_) ++gaps_detected_;
    recovering_ = true;
    waiting_for_snapshot_ = markets_;
    snapshot_seq_.clear();
    buffered_.clear();
    seqs_seen_while_recovering_.clear();

    std::vector<std::string> tickers(markets_.begin(), markets_.end());
    for (const std::string& ticker : tickers) {
        const InstrumentId id = instrument_for(ticker);
        if (id != kNoInstrument) emit(FeedEventType::BookStale, id, Side::Buy, Price{}, Qty{}, 0, receive_ns);
    }
    if (!tickers.empty()) request_snapshots_(tickers);
}

void KalshiBookBuilder::apply_snapshot(const KalshiSnapshotMessage& message, int64_t receive_ns) {
    auto& book = books_[message.market_ticker];
    if (!book) book = std::make_unique<MarketOrderBook>();
    book->clear();

    const InstrumentId id = instrument_for(message.market_ticker);
    const uint64_t seq = static_cast<uint64_t>(message.seq);
    if (id != kNoInstrument) emit(FeedEventType::BookClear, id, Side::Buy, Price{}, Qty{}, seq, receive_ns);
    for (const PriceLevel& level : message.book.bids) {
        book->set_level(Side::Buy, level.price, level.qty);
        if (id != kNoInstrument) emit(FeedEventType::BookLevel, id, Side::Buy, level.price, level.qty, seq, receive_ns);
    }
    for (const PriceLevel& level : message.book.asks) {
        book->set_level(Side::Sell, level.price, level.qty);
        if (id != kNoInstrument) emit(FeedEventType::BookLevel, id, Side::Sell, level.price, level.qty, seq, receive_ns);
    }
    if (id != kNoInstrument) emit(FeedEventType::BookSnapshotDone, id, Side::Buy, Price{}, Qty{}, seq, receive_ns);
}

bool KalshiBookBuilder::apply_delta(const KalshiDeltaMessage& message, int64_t receive_ns) {
    const KalshiBookDelta& delta = message.delta;
    auto it = books_.find(delta.market_ticker);
    if (it == books_.end()) return true;  // not a market we follow
    if (!it->second->apply_delta(delta.side, delta.price, delta.delta)) return false;  // went negative: out of sync

    const InstrumentId id = instrument_for(delta.market_ticker);
    if (id != kNoInstrument) {
        emit(FeedEventType::BookDelta, id, delta.side, delta.price, delta.delta, static_cast<uint64_t>(message.seq),
             receive_ns, delta.exchange_ns);
    }
    return true;
}

void KalshiBookBuilder::on_delta(const KalshiDeltaMessage& message, int64_t receive_ns) {
    if (recovering_) {
        buffered_.push_back(message);
        seqs_seen_while_recovering_.insert(message.seq);
        return;
    }
    if (last_seq_ != 0 && message.seq <= last_seq_) {
        ++duplicates_ignored_;  // already applied (e.g. a resend after reconnect)
        return;
    }
    if (last_seq_ != 0 && message.seq != last_seq_ + 1) {
        start_recovery(receive_ns, "sequence gap");
        buffered_.push_back(message);
        seqs_seen_while_recovering_.insert(message.seq);
        return;
    }
    last_seq_ = message.seq;
    if (!apply_delta(message, receive_ns)) start_recovery(receive_ns, "negative level");
}

void KalshiBookBuilder::on_snapshot(const KalshiSnapshotMessage& message, int64_t receive_ns) {
    if (!markets_.count(message.market_ticker)) return;
    apply_snapshot(message, receive_ns);

    if (recovering_) {
        seqs_seen_while_recovering_.insert(message.seq);
        snapshot_seq_[message.market_ticker] = message.seq;
        waiting_for_snapshot_.erase(message.market_ticker);
        if (waiting_for_snapshot_.empty()) finish_recovery(receive_ns);
        return;
    }
    // First snapshots of a new subscription; they take a seq like any other message.
    if (last_seq_ != 0 && message.seq != last_seq_ + 1 && message.seq > last_seq_) {
        // A hole right before a snapshot only matters for the *other* markets.
        if (markets_.size() > 1) {
            start_recovery(receive_ns, "sequence gap before snapshot");
            snapshot_seq_[message.market_ticker] = message.seq;
            waiting_for_snapshot_.erase(message.market_ticker);
            seqs_seen_while_recovering_.insert(message.seq);
            return;
        }
    }
    last_seq_ = std::max(last_seq_, message.seq);
}

void KalshiBookBuilder::finish_recovery(int64_t receive_ns) {
    if (snapshot_seq_.empty()) {  // every market was removed while we waited
        recovering_ = false;
        return;
    }

    // Snapshots cover everything up to the oldest one; after that the stream must have no hole.
    int64_t oldest_snapshot = snapshot_seq_.begin()->second;
    for (const auto& [ticker, seq] : snapshot_seq_) oldest_snapshot = std::min(oldest_snapshot, seq);
    const int64_t newest_seen = *seqs_seen_while_recovering_.rbegin();
    for (int64_t seq = oldest_snapshot + 1; seq <= newest_seen; ++seq) {
        if (!seqs_seen_while_recovering_.count(seq)) {
            ++failed_rounds_;
            start_recovery(receive_ns, "hole during recovery");
            return;
        }
    }

    // Replay, in sequence order, the deltas each snapshot doesn't include.
    std::sort(buffered_.begin(), buffered_.end(),
              [](const KalshiDeltaMessage& a, const KalshiDeltaMessage& b) { return a.seq < b.seq; });
    recovering_ = false;
    for (const KalshiDeltaMessage& message : buffered_) {
        const auto it = snapshot_seq_.find(message.delta.market_ticker);
        if (it == snapshot_seq_.end() || message.seq <= it->second) continue;
        if (!apply_delta(message, receive_ns)) {
            ++failed_rounds_;
            start_recovery(receive_ns, "negative level during replay");
            return;
        }
        ++deltas_replayed_;
    }
    buffered_.clear();
    seqs_seen_while_recovering_.clear();
    last_seq_ = newest_seen;
    failed_rounds_ = 0;
    ++recoveries_completed_;
}

}  // namespace mt
