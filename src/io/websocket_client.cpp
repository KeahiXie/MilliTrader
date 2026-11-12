#include "io/websocket_client.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include "core/clock.hpp"
#include "io/http_client.hpp"

namespace mt {

struct WebSocketClient::Impl {
    ix::WebSocket socket;
};

WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) { ix::initNetSystem(); }

WebSocketClient::~WebSocketClient() { close(); }

void WebSocketClient::connect(const std::string& url, const std::map<std::string, std::string>& headers,
                              Callbacks callbacks, int ping_interval_seconds) {
    ix::WebSocket& socket = impl_->socket;
    socket.setUrl(url);

    ix::WebSocketHttpHeaders extra_headers;
    // Portless Origin and our own User-Agent: Kalshi's firewall 403s IXWebSocket's defaults.
    std::string origin = url_origin(url);
    if (origin.rfind("wss://", 0) == 0) origin.replace(0, 3, "https");
    if (origin.rfind("ws://", 0) == 0) origin.replace(0, 2, "http");
    extra_headers["Origin"] = origin;
    extra_headers["User-Agent"] = "MilliTrader/1.0";
    for (const auto& [name, value] : headers) extra_headers[name] = value;
    socket.setExtraHeaders(extra_headers);

    ix::SocketTLSOptions tls;
    tls.caFile = "SYSTEM";
    socket.setTLSOptions(tls);

    socket.disableAutomaticReconnection();
    socket.setPingInterval(ping_interval_seconds);  // keeps NATs/proxies from dropping an idle connection
    // No permessage-deflate: costs CPU per frame and hides the venue's real bytes.
    socket.disablePerMessageDeflate();

    socket.setOnMessageCallback([callbacks = std::move(callbacks)](const ix::WebSocketMessagePtr& message) {
        switch (message->type) {
            case ix::WebSocketMessageType::Message:
                // Stamp the receive time first.
                if (callbacks.on_message) callbacks.on_message(wall_clock_ns(), message->str);
                break;
            case ix::WebSocketMessageType::Open:
                if (callbacks.on_open) callbacks.on_open();
                break;
            case ix::WebSocketMessageType::Close:
                if (callbacks.on_close) {
                    callbacks.on_close("closed: " + std::to_string(message->closeInfo.code) + " " +
                                       message->closeInfo.reason);
                }
                break;
            case ix::WebSocketMessageType::Error:
                if (callbacks.on_close) {
                    callbacks.on_close("error: " + message->errorInfo.reason + " (http " +
                                       std::to_string(message->errorInfo.http_status) + ")");
                }
                break;
            default:
                break;  // Ping / Pong / Fragment: nothing to do
        }
    });
    socket.start();
}

bool WebSocketClient::send_text(const std::string& text) {
    if (!is_open()) return false;
    return impl_->socket.sendText(text).success;
}

bool WebSocketClient::is_open() const { return impl_->socket.getReadyState() == ix::ReadyState::Open; }

void WebSocketClient::close() { impl_->socket.stop(); }

}  // namespace mt
