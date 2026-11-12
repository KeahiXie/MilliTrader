#include "io/http_client.hpp"

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>

namespace mt {

struct HttpClient::Impl {
    ix::HttpClient client{/*async=*/false};
};

HttpClient::HttpClient() : impl_(new Impl) {
    ix::initNetSystem();
    ix::SocketTLSOptions tls;
    tls.caFile = "SYSTEM";  // verify against the OS trust store
    impl_->client.setTLSOptions(tls);
}

HttpClient::~HttpClient() { delete impl_; }

HttpResponse HttpClient::get(const std::string& url, const HttpHeaders& headers) {
    return request("GET", url, "", headers);
}

HttpResponse HttpClient::post(const std::string& url, const std::string& json_body, const HttpHeaders& headers) {
    return request("POST", url, json_body, headers);
}

HttpResponse HttpClient::del(const std::string& url, const HttpHeaders& headers) {
    return request("DELETE", url, "", headers);
}

HttpResponse HttpClient::request(const std::string& method, const std::string& url, const std::string& body,
                                 const HttpHeaders& headers) {
    ix::HttpRequestArgsPtr args = impl_->client.createRequest(url, method);
    args->connectTimeout = 5;
    args->transferTimeout = 10;
    args->followRedirects = false;
    args->compress = false;  // keep bodies byte-identical to what the venue sent
    args->extraHeaders["Accept"] = "application/json";
    args->extraHeaders["User-Agent"] = "MilliTrader/1.0";
    // Send a portless Origin: IXWebSocket's default "https://host:443" gets a 403 from Kalshi's firewall.
    args->extraHeaders["Origin"] = url_origin(url);
    if (!body.empty()) args->extraHeaders["Content-Type"] = "application/json";
    for (const auto& [name, value] : headers) args->extraHeaders[name] = value;
    args->body = body;

    ix::HttpResponsePtr response = impl_->client.request(url, method, body, args);

    HttpResponse result;
    result.status = response->statusCode;
    result.body = response->body;
    if (response->errorCode != ix::HttpErrorCode::Ok) result.error = response->errorMsg;
    return result;
}

std::string url_origin(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    const size_t path_start = url.find('/', scheme + 3);
    return url.substr(0, path_start);
}

std::string url_path_without_query(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t path_start = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (path_start == std::string::npos) return "/";
    const size_t query = url.find('?', path_start);
    return url.substr(path_start, query == std::string::npos ? std::string::npos : query - path_start);
}

}  // namespace mt
