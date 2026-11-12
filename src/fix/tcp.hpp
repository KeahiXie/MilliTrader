#pragma once
// Small non-blocking TCP helpers shared by FixGateway (client side) and the
// mock exchange (server side). POSIX sockets only.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mt::fix {

// Non-blocking listening socket (port 0 = OS picks, see local_port). Returns -1 and sets `error` on failure.
int open_listen_socket(const std::string& address, uint16_t port, std::string& error);
uint16_t local_port(int fd);

// Accepts one waiting connection, with set_low_latency_options applied. Returns -1 if none is waiting.
int accept_connection(int listen_fd, std::string& peer_description);

// Waits at most timeout_ms for the handshake. Returns a non-blocking socket, or -1 and sets `error`.
int connect_tcp(const std::string& host, uint16_t port, int timeout_ms, std::string& error);

// O_NONBLOCK and TCP_NODELAY.
void set_low_latency_options(int fd);

// A connected non-blocking socket plus the bytes the kernel could not take yet.
class TcpConnection {
public:
    explicit TcpConnection(int fd) : fd_(fd) {}
    ~TcpConnection() { close(); }
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    int fd() const { return fd_; }
    bool is_open() const { return fd_ >= 0; }
    bool has_pending_output() const { return !pending_output_.empty(); }

    // Sends what the socket takes now and queues the rest, in order, for flush(). Closes on error.
    void write(std::string_view bytes);
    // Sends queued bytes; call on POLLOUT.
    void flush();

    enum class ReadStatus : uint8_t { Data, WouldBlock, Closed };
    struct ReadResult {
        ReadStatus status = ReadStatus::WouldBlock;
        size_t size = 0;
    };
    // One recv() into `buffer`.
    ReadResult read_some(char* buffer, size_t capacity);

    void close();

private:
    size_t send_now(std::string_view bytes);

    int fd_ = -1;
    std::string pending_output_;
};

}  // namespace mt::fix
