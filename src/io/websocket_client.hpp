#pragma once
// WebSocketClient: thin wrapper over IXWebSocket. Callbacks run on IXWebSocket's thread: copy data
// out (e.g. to an SPSC queue), never touch engine state there. Auto-reconnect is off because a
// reconnect needs freshly signed headers and a new book snapshot; the owner handles on_close.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace mt {

class WebSocketClient {
public:
    struct Callbacks {
        std::function<void()> on_open;
        std::function<void(int64_t receive_ns, const std::string& text)> on_message;
        std::function<void(const std::string& reason)> on_close;  // also called on connect failure
    };

    WebSocketClient();
    ~WebSocketClient();  // stops the connection and joins its thread

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Connects in the background.
    void connect(const std::string& url, const std::map<std::string, std::string>& headers, Callbacks callbacks,
                 int ping_interval_seconds = 20);
    bool send_text(const std::string& text);  // thread-safe; false if not connected
    bool is_open() const;
    void close();                              // blocking: returns after the thread stops

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mt
