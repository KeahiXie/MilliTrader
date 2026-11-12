#include "md/kalshi_rest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

#include "core/clock.hpp"
#include "io/auth.hpp"

namespace mt {

using nlohmann::json;

namespace {

std::string string_field(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// [["0.0010","818538.54"], ...] -> levels. Returns false on any bad entry.
bool parse_level_array(const json& array, std::vector<PriceLevel>* levels) {
    if (array.is_null()) return true;  // an empty side is sent as null
    if (!array.is_array()) return false;
    for (const json& entry : array) {
        if (!entry.is_array() || entry.size() != 2 || !entry[0].is_string() || !entry[1].is_string()) return false;
        const auto price = parse_price(entry[0].get<std::string>());
        const auto qty = parse_qty(entry[1].get<std::string>());
        if (!price || !qty) return false;
        levels->push_back({*price, *qty});
    }
    return true;
}

}  // namespace

BookSnapshot kalshi_bids_to_book(const std::vector<PriceLevel>& yes_bids, const std::vector<PriceLevel>& no_bids) {
    BookSnapshot book;
    book.bids = yes_bids;
    book.asks.reserve(no_bids.size());
    for (const PriceLevel& level : no_bids) book.asks.push_back({complement(level.price), level.qty});

    std::sort(book.bids.begin(), book.bids.end(), [](const auto& a, const auto& b) { return a.price > b.price; });
    std::sort(book.asks.begin(), book.asks.end(), [](const auto& a, const auto& b) { return a.price < b.price; });
    return book;
}

std::optional<std::vector<KalshiMarket>> parse_kalshi_markets(const std::string& json_text, std::string* next_cursor) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object() || !root.contains("markets") || !root["markets"].is_array()) return std::nullopt;
    if (next_cursor) *next_cursor = string_field(root, "cursor");

    std::vector<KalshiMarket> markets;
    for (const json& item : root["markets"]) {
        KalshiMarket market;
        market.ticker = string_field(item, "ticker");
        market.event_ticker = string_field(item, "event_ticker");
        market.status = string_field(item, "status");
        market.open_time_ns = parse_utc_iso8601(string_field(item, "open_time"));
        market.close_time_ns = parse_utc_iso8601(string_field(item, "close_time"));
        market.price_level_structure = string_field(item, "price_level_structure");
        if (item.contains("floor_strike") && item["floor_strike"].is_number()) {
            market.floor_strike = item["floor_strike"].dump();
        }
        if (item.contains("price_ranges") && item["price_ranges"].is_array()) {
            for (const json& range : item["price_ranges"]) {
                const auto start = parse_price(string_field(range, "start"));
                const auto end = parse_price(string_field(range, "end"));
                const auto step = parse_price(string_field(range, "step"));
                if (start && end && step) market.tick_grid.push_back({*start, *end, *step});
            }
        }
        if (market.tick_grid.empty()) market.tick_grid = one_cent_grid();
        if (!market.ticker.empty()) markets.push_back(std::move(market));
    }
    return markets;
}

std::optional<BookSnapshot> parse_kalshi_orderbook(const std::string& json_text) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object() || !root.contains("orderbook_fp")) return std::nullopt;
    const json& book = root["orderbook_fp"];

    std::vector<PriceLevel> yes_bids, no_bids;
    if (!parse_level_array(book.value("yes_dollars", json()), &yes_bids)) return std::nullopt;
    if (!parse_level_array(book.value("no_dollars", json()), &no_bids)) return std::nullopt;
    return kalshi_bids_to_book(yes_bids, no_bids);
}

KalshiRestClient::KalshiRestClient(std::string base_url, const KalshiSigner* signer)
    : base_url_(std::move(base_url)), signer_(signer) {}

HttpHeaders KalshiRestClient::auth_headers(const std::string& method, const std::string& full_url) {
    if (signer_ == nullptr) return {};
    const int64_t timestamp_ms = wall_clock_ns() / kNanosPerMilli;
    return signer_->headers(timestamp_ms, method, url_path_without_query(full_url));
}

std::optional<std::vector<KalshiMarket>> KalshiRestClient::list_markets(const std::string& series_ticker,
                                                                        const std::string& status,
                                                                        int64_t min_close_ts, int64_t max_close_ts) {
    std::string url = base_url_ + "/markets?limit=200&series_ticker=" + series_ticker;
    if (!status.empty()) url += "&status=" + status;
    if (min_close_ts > 0) url += "&min_close_ts=" + std::to_string(min_close_ts);
    if (max_close_ts > 0) url += "&max_close_ts=" + std::to_string(max_close_ts);

    const HttpResponse response = http_.get(url, auth_headers("GET", url));
    if (!response.ok()) return std::nullopt;
    return parse_kalshi_markets(response.body);
}

std::optional<std::vector<KalshiMarket>> KalshiRestClient::list_markets_by_ticker(const std::vector<std::string>& tickers) {
    std::string joined;
    for (const std::string& ticker : tickers) joined += (joined.empty() ? "" : ",") + ticker;
    const std::string url = base_url_ + "/markets?tickers=" + joined;
    const HttpResponse response = http_.get(url, auth_headers("GET", url));
    if (!response.ok()) return std::nullopt;
    return parse_kalshi_markets(response.body);
}

KalshiRestClient::OrderbookResult KalshiRestClient::get_orderbook(const std::string& ticker) {
    const std::string url = base_url_ + "/markets/" + ticker + "/orderbook";
    const HttpResponse response = http_.get(url, auth_headers("GET", url));
    OrderbookResult result;
    result.http_status = response.status;
    result.raw_body = response.body;
    if (response.ok()) result.book = parse_kalshi_orderbook(response.body);
    return result;
}

HttpResponse KalshiRestClient::signed_get(const std::string& path_and_query) {
    const std::string url = base_url_ + path_and_query;
    return http_.get(url, auth_headers("GET", url));
}

HttpResponse KalshiRestClient::signed_post(const std::string& path, const std::string& json_body) {
    const std::string url = base_url_ + path;
    return http_.post(url, json_body, auth_headers("POST", url));
}

HttpResponse KalshiRestClient::signed_delete(const std::string& path) {
    const std::string url = base_url_ + path;
    return http_.del(url, auth_headers("DELETE", url));
}

}  // namespace mt
