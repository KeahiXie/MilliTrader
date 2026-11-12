#pragma once
// Kalshi WebSocket protocol: command builders and message parsers.
// Kept in one file because these JSON shapes are what changes when Kalshi revises its API.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "md/kalshi_rest.hpp"

namespace mt {

inline const char* kKalshiWsSignPath = "/trade-api/ws/v2";

// {"id":1,"cmd":"subscribe","params":{"channels":["orderbook_delta"],"market_tickers":[...]}}
// Empty ticker list = every market (allowed for ticker/trade/lifecycle).
std::string kalshi_subscribe_command(int64_t id, const std::vector<std::string>& channels,
                                     const std::vector<std::string>& market_tickers);

// {"id":2,"cmd":"update_subscription","params":{"sids":[5],"market_tickers":[...],"action":"add_markets"}}
// action: "add_markets" or "delete_markets".
std::string kalshi_update_subscription_command(int64_t id, int64_t sid, const std::vector<std::string>& market_tickers,
                                               const std::string& action);

// {"id":3,"cmd":"update_subscription","params":{"sids":[5],"market_tickers":[...],"action":"get_snapshot"}}
// Fresh snapshots without changing the subscription (used for gap recovery).
std::string kalshi_get_snapshot_command(int64_t id, int64_t sid, const std::vector<std::string>& market_tickers);

// {"id":4,"cmd":"unsubscribe","params":{"sids":[5]}}
std::string kalshi_unsubscribe_command(int64_t id, const std::vector<int64_t>& sids);

// Routing fields common to every Kalshi message.
struct KalshiMessageHeader {
    std::string type;           // "orderbook_snapshot", "orderbook_delta", "subscribed", "error", ...
    int64_t id = 0;             // echo of our command id (responses only)
    int64_t sid = 0;            // subscription id (0 if absent)
    int64_t seq = 0;            // per-subscription sequence number (0 if absent)
    std::string channel;        // for "subscribed": which channel the sid belongs to
    std::string market_ticker;  // for book messages
    int64_t error_code = 0;
    std::string error_message;
};

std::optional<KalshiMessageHeader> parse_kalshi_header(const std::string& text);

// One book change in YES terms (a NO bid at p is a YES ask at 1 - p).
struct KalshiBookDelta {
    std::string market_ticker;
    Side side = Side::Buy;      // Buy = YES bid level changed, Sell = YES ask (NO bid) changed
    Price price{};              // in YES terms
    Qty delta{};
    int64_t exchange_ns = 0;
};

std::optional<BookSnapshot> parse_kalshi_ws_snapshot(const std::string& text, std::string* market_ticker);
std::optional<KalshiBookDelta> parse_kalshi_ws_delta(const std::string& text);

}  // namespace mt
