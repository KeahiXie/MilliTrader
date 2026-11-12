#include "md/kalshi_ws_protocol.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

#include "core/clock.hpp"

namespace mt {

using nlohmann::json;

std::string kalshi_subscribe_command(int64_t id, const std::vector<std::string>& channels,
                                     const std::vector<std::string>& market_tickers) {
    json params = {{"channels", channels}};
    if (!market_tickers.empty()) params["market_tickers"] = market_tickers;
    // Ask for NO levels in NO prices explicitly, since Kalshi plans to flip the default.
    // complement() turns them into YES asks.
    if (std::find(channels.begin(), channels.end(), "orderbook_delta") != channels.end()) {
        params["use_yes_price"] = false;
    }
    return json{{"id", id}, {"cmd", "subscribe"}, {"params", params}}.dump();
}

std::string kalshi_update_subscription_command(int64_t id, int64_t sid, const std::vector<std::string>& market_tickers,
                                               const std::string& action) {
    const json params = {{"sids", {sid}}, {"market_tickers", market_tickers}, {"action", action}};
    return json{{"id", id}, {"cmd", "update_subscription"}, {"params", params}}.dump();
}

std::string kalshi_get_snapshot_command(int64_t id, int64_t sid, const std::vector<std::string>& market_tickers) {
    return kalshi_update_subscription_command(id, sid, market_tickers, "get_snapshot");
}

std::string kalshi_unsubscribe_command(int64_t id, const std::vector<int64_t>& sids) {
    return json{{"id", id}, {"cmd", "unsubscribe"}, {"params", {{"sids", sids}}}}.dump();
}

std::optional<KalshiMessageHeader> parse_kalshi_header(const std::string& text) {
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object()) return std::nullopt;

    KalshiMessageHeader header;
    header.type = root.value("type", "");
    if (root.contains("id") && root["id"].is_number_integer()) header.id = root["id"].get<int64_t>();
    if (root.contains("sid") && root["sid"].is_number_integer()) header.sid = root["sid"].get<int64_t>();
    if (root.contains("seq") && root["seq"].is_number_integer()) header.seq = root["seq"].get<int64_t>();

    if (root.contains("msg") && root["msg"].is_object()) {
        const json& msg = root["msg"];
        header.channel = msg.value("channel", "");
        header.market_ticker = msg.value("market_ticker", "");
        if (header.sid == 0 && msg.contains("sid") && msg["sid"].is_number_integer()) {
            header.sid = msg["sid"].get<int64_t>();  // "subscribed" puts sid inside msg
        }
        if (header.type == "error") {
            if (msg.contains("code") && msg["code"].is_number_integer()) header.error_code = msg["code"].get<int64_t>();
            header.error_message = msg.value("msg", "");
        }
    }
    return header;
}

namespace {

// Accepts all three level formats Kalshi has used (<side>_dollars_fp, <side>_dollars, integer cents)
// so an API revision doesn't silently empty the book.
bool parse_levels(const json& msg, const char* side, std::vector<PriceLevel>* levels) {
    const std::string base(side);
    for (const std::string& key : {base + "_dollars_fp", base + "_dollars", base}) {
        if (!msg.contains(key)) continue;
        const json& array = msg[key];
        if (array.is_null()) return true;
        if (!array.is_array()) return false;
        for (const json& entry : array) {
            if (!entry.is_array() || entry.size() != 2) return false;
            std::optional<Price> price;
            std::optional<Qty> qty;
            if (entry[0].is_string()) price = parse_price(entry[0].get<std::string>());
            if (entry[0].is_number_integer()) price = Price{static_cast<int32_t>(entry[0].get<int64_t>() * 100)};
            if (entry[1].is_string()) qty = parse_qty(entry[1].get<std::string>());
            if (entry[1].is_number_integer()) qty = contracts(entry[1].get<int64_t>());
            if (!price || !qty) return false;
            levels->push_back({*price, *qty});
        }
        return true;
    }
    return true;  // side absent = empty
}

}  // namespace

std::optional<BookSnapshot> parse_kalshi_ws_snapshot(const std::string& text, std::string* market_ticker) {
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object() || root.value("type", "") != "orderbook_snapshot" || !root.contains("msg")) {
        return std::nullopt;
    }
    const json& msg = root["msg"];
    if (market_ticker) *market_ticker = msg.value("market_ticker", "");
    std::vector<PriceLevel> yes_bids, no_bids;
    if (!parse_levels(msg, "yes", &yes_bids) || !parse_levels(msg, "no", &no_bids)) return std::nullopt;
    return kalshi_bids_to_book(yes_bids, no_bids);
}

std::optional<KalshiBookDelta> parse_kalshi_ws_delta(const std::string& text) {
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object() || root.value("type", "") != "orderbook_delta" || !root.contains("msg")) {
        return std::nullopt;
    }
    const json& msg = root["msg"];
    KalshiBookDelta delta;
    delta.market_ticker = msg.value("market_ticker", "");

    std::optional<Price> price;
    if (msg.contains("price_dollars") && msg["price_dollars"].is_string()) {
        price = parse_price(msg["price_dollars"].get<std::string>());
    } else if (msg.contains("price") && msg["price"].is_number_integer()) {
        price = Price{static_cast<int32_t>(msg["price"].get<int64_t>() * 100)};
    }
    std::optional<Qty> qty;
    if (msg.contains("delta_fp") && msg["delta_fp"].is_string()) {
        qty = parse_qty(msg["delta_fp"].get<std::string>());
    } else if (msg.contains("delta") && msg["delta"].is_number_integer()) {
        qty = contracts(msg["delta"].get<int64_t>());
    }
    const std::string side = msg.value("side", "");
    if (!price || !qty || (side != "yes" && side != "no")) return std::nullopt;

    // A change to the NO bids at p is a change to the YES asks at 1 - p.
    delta.side = side == "yes" ? Side::Buy : Side::Sell;
    delta.price = side == "yes" ? *price : complement(*price);
    delta.delta = *qty;
    if (msg.contains("ts") && msg["ts"].is_string()) delta.exchange_ns = parse_utc_iso8601(msg["ts"].get<std::string>());
    return delta;
}

}  // namespace mt
