#pragma once
// MarketDataConsumer: Kalshi market data for the trader, as FeedEvents on one SPSC queue to the engine.
// WebSocket mode (needs a key) goes through KalshiBookBuilder; REST mode sends full snapshots on the same path.
// Discovery runs on the supervisor; market changes reach the WebSocket thread via a small locked inbox (rare).

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/events.hpp"
#include "core/instruments.hpp"
#include "core/spsc_queue.hpp"
#include "io/auth.hpp"
#include "io/websocket_client.hpp"
#include "md/kalshi_book_builder.hpp"
#include "md/kalshi_rest.hpp"

namespace mt {

using FeedQueue = SpscQueue<FeedEvent>;

class MarketDataConsumer {
public:
    struct Config {
        std::string series = "KXBTC15M";
        std::vector<std::string> market_tickers;  // if set: exactly these markets, any series
        bool use_websocket = false;
        double rest_polls_per_second = 4.0;
        std::string rest_url = kKalshiProdRestUrl;
        std::string ws_url = kKalshiProdWsUrl;
        int64_t drop_delta_number = 0;  // > 0: drop that delta to force a sequence gap
    };

    MarketDataConsumer(Config config, InstrumentTable& instruments, FeedQueue& out, const KalshiSigner* signer);
    ~MarketDataConsumer();

    void start();
    void maintain(int64_t now_ns);  // supervisor thread, a few times a second
    void stop();

    int64_t events() const { return events_.load(std::memory_order_relaxed); }
    int64_t gaps() const { return gaps_.load(std::memory_order_relaxed); }
    int64_t recoveries() const { return recoveries_.load(std::memory_order_relaxed); }
    int64_t last_event_wall_ns() const { return last_event_ns_.load(std::memory_order_relaxed); }
    int64_t deltas_dropped_on_purpose() const { return dropped_on_purpose_.load(std::memory_order_relaxed); }

private:
    struct MarketChange {
        bool add = true;
        std::string ticker;
    };

    void emit(const FeedEvent& event);
    std::vector<KalshiMarket> discover(int64_t now_ns);
    InstrumentId register_market(const KalshiMarket& market);

    // REST mode
    void rest_loop();
    void emit_snapshot(InstrumentId id, const BookSnapshot& book, int64_t receive_ns, int64_t receive_mono_ns);

    // WebSocket mode
    void ws_connect(int64_t now_ns);
    void ws_on_message(int64_t receive_ns, const std::string& text);
    void apply_market_changes();  // WebSocket thread

    Config config_;
    InstrumentTable& instruments_;
    FeedQueue& out_;
    const KalshiSigner* signer_;
    KalshiRestClient rest_;

    std::atomic<bool> running_{false};
    std::thread rest_thread_;

    // WebSocket state
    WebSocketClient socket_;
    std::unique_ptr<KalshiBookBuilder> builder_;   // used only on the WebSocket thread
    std::atomic<bool> ws_open_{false};
    std::atomic<bool> ws_failed_{false};
    std::atomic<int64_t> book_sid_{0};
    std::atomic<int64_t> next_command_id_{1};
    bool ws_connecting_ = false;
    bool ws_subscribed_ = false;
    int64_t ws_next_connect_ns_ = 0;
    int64_t ws_retry_delay_ns_ = 0;
    std::vector<std::string> subscribed_markets_;  // supervisor only
    std::mutex inbox_mutex_;
    std::vector<MarketChange> inbox_;
    std::atomic<bool> inbox_pending_{false};
    int64_t deltas_seen_ = 0;                      // WebSocket thread only

    // Supervisor
    int64_t last_discovery_ns_ = 0;
    std::vector<std::string> wanted_markets_;

    std::atomic<int64_t> events_{0};
    std::atomic<int64_t> gaps_{0};
    std::atomic<int64_t> recoveries_{0};
    std::atomic<int64_t> last_event_ns_{0};
    std::atomic<int64_t> dropped_on_purpose_{0};
};

}  // namespace mt
