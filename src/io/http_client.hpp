#pragma once
// Small blocking HTTPS client over IXWebSocket's HttpClient. Off the hot path only
// (market discovery, REST book polling, order entry on the gateway thread).

#include <map>
#include <string>

namespace mt {

struct HttpResponse {
    int status = 0;          // HTTP status, or 0 if the request never completed
    std::string body;
    std::string error;       // transport error text (DNS, TLS, timeout)

    bool ok() const { return status >= 200 && status < 300; }
};

using HttpHeaders = std::map<std::string, std::string>;

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse get(const std::string& url, const HttpHeaders& headers = {});
    HttpResponse post(const std::string& url, const std::string& json_body, const HttpHeaders& headers = {});
    HttpResponse del(const std::string& url, const HttpHeaders& headers = {});

private:
    HttpResponse request(const std::string& method, const std::string& url, const std::string& body,
                         const HttpHeaders& headers);

    struct Impl;  // keeps IXWebSocket headers out of this header
    Impl* impl_;
};

// "https://host/trade-api/v2/markets?x=1" -> "https://host"
std::string url_origin(const std::string& url);

// "https://host/trade-api/v2/markets?x=1" -> "/trade-api/v2/markets" (what Kalshi signs).
std::string url_path_without_query(const std::string& url);

}  // namespace mt
