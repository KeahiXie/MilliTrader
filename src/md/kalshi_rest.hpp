#pragma once
// Kalshi REST market data: market discovery and order-book snapshots.
// Kalshi only sends YES and NO bids. Buying NO at p is selling YES at 1 - p, so NO bids become YES asks here.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "io/http_client.hpp"

namespace mt {

class KalshiSigner;

inline const char* kKalshiProdRestUrl = "https://external-api.kalshi.com/trade-api/v2";
inline const char* kKalshiDemoRestUrl = "https://external-api.demo.kalshi.co/trade-api/v2";
inline const char* kKalshiProdWsUrl = "wss://external-api-ws.kalshi.com/trade-api/ws/v2";
inline const char* kKalshiDemoWsUrl = "wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2";

struct PriceLevel {
    Price price;
    Qty qty;
};

// Bids best (highest) first, asks best (lowest) first.
struct BookSnapshot {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

struct KalshiMarket {
    std::string ticker;
    std::string event_ticker;
    std::string status;            // "active", "initialized", "closed", ...
    int64_t open_time_ns = 0;
    int64_t close_time_ns = 0;
    std::vector<PriceRange> tick_grid;
    std::string price_level_structure;  // e.g. "tapered_deci_cent"
    std::string floor_strike;      // BTC target price as text, if the series has one
};

// Pure parsers; nullopt on malformed input.
std::optional<std::vector<KalshiMarket>> parse_kalshi_markets(const std::string& json_text,
                                                              std::string* next_cursor = nullptr);
std::optional<BookSnapshot> parse_kalshi_orderbook(const std::string& json_text);

// YES/NO bid lists -> YES bid/ask book. Shared by the REST and WebSocket snapshot parsers.
BookSnapshot kalshi_bids_to_book(const std::vector<PriceLevel>& yes_bids, const std::vector<PriceLevel>& no_bids);

class KalshiRestClient {
public:
    // `signer` may be null: market data endpoints are public.
    KalshiRestClient(std::string base_url, const KalshiSigner* signer);

    // GET /markets?series_ticker=...&status=... (status may be empty).
    // Close filters are Unix seconds, 0 = no filter.
    std::optional<std::vector<KalshiMarket>> list_markets(const std::string& series_ticker, const std::string& status,
                                                          int64_t min_close_ts = 0, int64_t max_close_ts = 0);

    // GET /markets?tickers=A,B (specific markets, any series)
    std::optional<std::vector<KalshiMarket>> list_markets_by_ticker(const std::vector<std::string>& tickers);

    // GET /markets/{ticker}/orderbook. Keeps the raw body because the recorder stores raw bytes.
    struct OrderbookResult {
        int http_status = 0;
        std::string raw_body;
        std::optional<BookSnapshot> book;
    };
    OrderbookResult get_orderbook(const std::string& ticker);

    // Signed request helpers used by the order gateway.
    HttpResponse signed_get(const std::string& path_and_query);
    HttpResponse signed_post(const std::string& path, const std::string& json_body);
    HttpResponse signed_delete(const std::string& path);

    const std::string& base_url() const { return base_url_; }

private:
    HttpHeaders auth_headers(const std::string& method, const std::string& full_url);

    std::string base_url_;
    const KalshiSigner* signer_;
    HttpClient http_;
};

}  // namespace mt
