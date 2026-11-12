#include "md/market_data_consumer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "core/clock.hpp"
#include "core/thread_utils.hpp"
#include "md/kalshi_ws_protocol.hpp"

namespace mt {

namespace {
constexpr int64_t kDiscoveryInterval = 30 * kNanosPerSecond;
constexpr int64_t kWindowNs = 15 * 60 * kNanosPerSecond;
constexpr int64_t kMaxRetryDelay = 60 * kNanosPerSecond;
}  // namespace

MarketDataConsumer::MarketDataConsumer(Config config, InstrumentTable& instruments, FeedQueue& out,
                                       const KalshiSigner* signer)
    : config_(std::move(config)), instruments_(instruments), out_(out), signer_(signer), rest_(config_.rest_url, nullptr) {
    if (config_.use_websocket && signer_ == nullptr) config_.use_websocket = false;  // no key: REST only
}

MarketDataConsumer::~MarketDataConsumer() { stop(); }

void MarketDataConsumer::emit(const FeedEvent& event) {
    FeedEvent stamped = event;
    stamped.enqueue_mono_ns = monotonic_ns();
    // Spin instead of dropping: a lost book update would corrupt the engine's book.
    while (!out_.try_push(stamped)) cpu_relax();
    events_.fetch_add(1, std::memory_order_relaxed);
    last_event_ns_.store(event.receive_ns, std::memory_order_relaxed);
}

InstrumentId MarketDataConsumer::register_market(const KalshiMarket& market) {
    return instruments_.add(
        make_instrument(Venue::Kalshi, market.ticker, market.tick_grid, market.close_time_ns, market.open_time_ns));
}

std::vector<KalshiMarket> MarketDataConsumer::discover(int64_t now_ns) {
    last_discovery_ns_ = now_ns;
    const int64_t now_s = now_ns / kNanosPerSecond;
    auto markets = config_.market_tickers.empty() ? rest_.list_markets(config_.series, "", now_s, now_s + 35 * 60)
                                                  : rest_.list_markets_by_ticker(config_.market_tickers);
    if (!markets) return {};
    std::vector<KalshiMarket> live;
    for (const KalshiMarket& market : *markets) {
        if (market.close_time_ns <= now_ns) continue;
        register_market(market);
        live.push_back(market);
    }
    return live;
}

void MarketDataConsumer::start() {
    running_ = true;
    if (config_.use_websocket) {
        builder_ = std::make_unique<KalshiBookBuilder>(
            instruments_, [this](const FeedEvent& event) { emit(event); },
            [this](const std::vector<std::string>& tickers) {
                // Runs on the WebSocket thread, inside on_message: ask for fresh books.
                gaps_.fetch_add(1, std::memory_order_relaxed);
                socket_.send_text(kalshi_get_snapshot_command(next_command_id_++, book_sid_.load(), tickers));
            });
        std::fprintf(stderr, "md: Kalshi WebSocket order books for %s\n", config_.series.c_str());
    } else {
        std::fprintf(stderr, "md: Kalshi REST snapshots for %s at %.1f/s (no API key)\n", config_.series.c_str(),
                     config_.rest_polls_per_second);
        rest_thread_ = start_thread("mt-feed", -1, [this] { rest_loop(); });
    }
}

void MarketDataConsumer::stop() {
    if (!running_.exchange(false)) return;
    if (rest_thread_.joinable()) rest_thread_.join();
    socket_.close();
}

// REST mode
void MarketDataConsumer::emit_snapshot(InstrumentId id, const BookSnapshot& book, int64_t receive_ns,
                                       int64_t receive_mono_ns) {
    FeedEvent event;
    event.venue = Venue::Kalshi;
    event.instrument = id;
    event.receive_ns = receive_ns;
    event.receive_mono_ns = receive_mono_ns;
    event.type = FeedEventType::BookClear;
    emit(event);
    event.type = FeedEventType::BookLevel;
    for (const PriceLevel& level : book.bids) {
        event.side = Side::Buy;
        event.price = level.price;
        event.qty = level.qty;
        emit(event);
    }
    for (const PriceLevel& level : book.asks) {
        event.side = Side::Sell;
        event.price = level.price;
        event.qty = level.qty;
        emit(event);
    }
    event.type = FeedEventType::BookSnapshotDone;
    event.qty = Qty{};
    emit(event);
}

void MarketDataConsumer::rest_loop() {
    const int64_t period = static_cast<int64_t>(kNanosPerSecond / std::max(0.1, config_.rest_polls_per_second));
    std::vector<KalshiMarket> markets;
    int64_t next_poll = wall_clock_ns();
    while (running_) {
        const int64_t now = wall_clock_ns();
        const int64_t boundary = now / kWindowNs * kWindowNs;
        if (markets.empty() || now - last_discovery_ns_ > kDiscoveryInterval ||
            (last_discovery_ns_ < boundary && now > boundary + kNanosPerSecond / 2)) {
            auto found = discover(now);
            if (!found.empty()) markets = std::move(found);
        }
        for (const KalshiMarket& market : markets) {
            if (now < market.open_time_ns || now >= market.close_time_ns) continue;
            const InstrumentId id = instruments_.find_id(Venue::Kalshi, market.ticker);
            if (id == kNoInstrument) continue;
            const auto result = rest_.get_orderbook(market.ticker);
            const int64_t received = wall_clock_ns();
            const int64_t received_mono = monotonic_ns();
            if (result.book) emit_snapshot(id, *result.book, received, received_mono);
        }
        next_poll = std::max(next_poll + period, wall_clock_ns() - period);
        const int64_t wait = next_poll - wall_clock_ns();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
    }
}

// WebSocket mode
void MarketDataConsumer::ws_connect(int64_t now_ns) {
    socket_.close();
    ws_open_ = false;
    ws_failed_ = false;
    ws_subscribed_ = false;
    subscribed_markets_.clear();
    book_sid_ = 0;

    WebSocketClient::Callbacks callbacks;
    callbacks.on_open = [this] { ws_open_ = true; };
    callbacks.on_message = [this](int64_t receive_ns, const std::string& text) { ws_on_message(receive_ns, text); };
    callbacks.on_close = [this](const std::string& reason) {
        std::fprintf(stderr, "md: websocket closed (%s)\n", reason.c_str());
        ws_open_ = false;
        ws_failed_ = true;
        // New connection = new subscription: every book is stale until its new snapshot.
        if (builder_) builder_->reset_for_new_subscription();
    };
    ws_connecting_ = true;
    socket_.connect(config_.ws_url, signer_->headers(now_ns / kNanosPerMilli, "GET", kKalshiWsSignPath), std::move(callbacks));
}

void MarketDataConsumer::apply_market_changes() {
    if (!inbox_pending_.exchange(false)) return;
    std::vector<MarketChange> changes;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        changes.swap(inbox_);
    }
    for (const MarketChange& change : changes) {
        if (change.add) builder_->add_market(change.ticker); else builder_->remove_market(change.ticker);
    }
}

void MarketDataConsumer::ws_on_message(int64_t receive_ns, const std::string& text) {
    const int64_t receive_mono = monotonic_ns();
    apply_market_changes();
    const auto header = parse_kalshi_header(text);
    if (!header) return;
    builder_->set_receive_mono_ns(receive_mono);

    if (header->type == "subscribed" && header->channel == "orderbook_delta") {
        book_sid_ = header->sid;
    } else if (header->type == "orderbook_snapshot") {
        KalshiSnapshotMessage message;
        message.sid = header->sid;
        message.seq = header->seq;
        const auto book = parse_kalshi_ws_snapshot(text, &message.market_ticker);
        if (!book) return;
        message.book = *book;
        const int64_t completed_before = builder_->recoveries_completed();
        builder_->on_snapshot(message, receive_ns);
        if (builder_->recoveries_completed() > completed_before) recoveries_.fetch_add(1, std::memory_order_relaxed);
    } else if (header->type == "orderbook_delta") {
        const auto delta = parse_kalshi_ws_delta(text);
        if (!delta) return;
        ++deltas_seen_;
        if (config_.drop_delta_number > 0 && deltas_seen_ == config_.drop_delta_number) {
            dropped_on_purpose_.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "md: dropping delta #%lld (seq %lld) on purpose to force a gap\n",
                         static_cast<long long>(deltas_seen_), static_cast<long long>(header->seq));
            return;
        }
        KalshiDeltaMessage message;
        message.sid = header->sid;
        message.seq = header->seq;
        message.delta = *delta;
        builder_->on_delta(message, receive_ns);
    } else if (header->type == "error") {
        std::fprintf(stderr, "md: kalshi error %lld: %s\n", static_cast<long long>(header->error_code),
                     header->error_message.c_str());
    }
}

void MarketDataConsumer::maintain(int64_t now_ns) {
    if (!config_.use_websocket || !running_) return;

    if (ws_failed_) {
        socket_.close();
        ws_failed_ = false;
        ws_connecting_ = false;
        ws_retry_delay_ns_ = std::clamp<int64_t>(ws_retry_delay_ns_ * 2, kNanosPerSecond, kMaxRetryDelay);
        ws_next_connect_ns_ = now_ns + ws_retry_delay_ns_;
    }
    if (!ws_connecting_ && now_ns >= ws_next_connect_ns_) ws_connect(now_ns);

    const int64_t boundary = now_ns / kWindowNs * kWindowNs;
    if (wanted_markets_.empty() || now_ns - last_discovery_ns_ > kDiscoveryInterval ||
        (last_discovery_ns_ < boundary && now_ns > boundary + kNanosPerSecond / 2)) {
        std::vector<std::string> wanted;
        for (const KalshiMarket& market : discover(now_ns)) wanted.push_back(market.ticker);
        std::sort(wanted.begin(), wanted.end());
        if (!wanted.empty()) wanted_markets_ = wanted;
    }
    if (!ws_open_ || wanted_markets_.empty()) return;

    // Post market changes before subscribing, so the builder knows a market before its first snapshot.
    std::vector<std::string> to_add, to_remove;
    for (const auto& ticker : wanted_markets_) {
        if (std::find(subscribed_markets_.begin(), subscribed_markets_.end(), ticker) == subscribed_markets_.end()) to_add.push_back(ticker);
    }
    for (const auto& ticker : subscribed_markets_) {
        if (std::find(wanted_markets_.begin(), wanted_markets_.end(), ticker) == wanted_markets_.end()) to_remove.push_back(ticker);
    }
    if (to_add.empty() && to_remove.empty() && ws_subscribed_) return;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        for (const auto& ticker : (ws_subscribed_ ? to_add : wanted_markets_)) inbox_.push_back({true, ticker});
        for (const auto& ticker : to_remove) inbox_.push_back({false, ticker});
        inbox_pending_ = true;
    }
    if (!ws_subscribed_) {
        socket_.send_text(kalshi_subscribe_command(next_command_id_++, {"orderbook_delta"}, wanted_markets_));
        ws_subscribed_ = true;
        ws_retry_delay_ns_ = 0;
    } else if (book_sid_ != 0) {
        if (!to_add.empty()) socket_.send_text(kalshi_update_subscription_command(next_command_id_++, book_sid_, to_add, "add_markets"));
        if (!to_remove.empty()) socket_.send_text(kalshi_update_subscription_command(next_command_id_++, book_sid_, to_remove, "delete_markets"));
    } else {
        return;  // wait for the "subscribed" answer before updating
    }
    subscribed_markets_ = wanted_markets_;
}

}  // namespace mt
