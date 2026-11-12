#include "fix/tcp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mt::fix {

namespace {

#ifdef MSG_NOSIGNAL
// Get EPIPE instead of a process-killing SIGPIPE when the peer has closed.
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// If a peer stops reading, do not buffer for it forever.
constexpr size_t kMaxPendingOutput = 8 * 1024 * 1024;

std::string errno_text() { return std::strerror(errno); }

}  // namespace

void set_low_latency_options(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // TCP_NODELAY disables Nagle: a small order write could otherwise wait for the peer's delayed
    // ACK (up to ~40 ms). We always write whole messages, so coalescing gains nothing.
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

int open_listen_socket(const std::string& address, uint16_t port, std::string& error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket: " + errno_text();
        return -1;
    }
    // Rebind right after a restart instead of waiting out TIME_WAIT.
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) {
        error = "bad bind address " + address;
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 || ::listen(fd, 16) < 0) {
        error = "bind/listen on " + address + ":" + std::to_string(port) + ": " + errno_text();
        ::close(fd);
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

uint16_t local_port(int fd) {
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) < 0) return 0;
    return ntohs(local.sin_port);
}

int accept_connection(int listen_fd, std::string& peer_description) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    const int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &length);
    if (fd < 0) return -1;  // EAGAIN: nobody waiting
    set_low_latency_options(fd);
    char address[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
    peer_description = std::string(address) + ":" + std::to_string(ntohs(peer.sin_port));
    return fd;
}

int connect_tcp(const std::string& host, uint16_t port, int timeout_ms, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const std::string port_text = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &found); rc != 0) {
        error = "cannot resolve " + host + ": " + ::gai_strerror(rc);
        return -1;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket: " + errno_text();
        ::freeaddrinfo(found);
        return -1;
    }
    set_low_latency_options(fd);
    const int rc = ::connect(fd, found->ai_addr, found->ai_addrlen);
    ::freeaddrinfo(found);
    if (rc == 0) return fd;
    if (errno != EINPROGRESS) {
        error = "connect to " + host + ":" + port_text + ": " + errno_text();
        ::close(fd);
        return -1;
    }

    // Non-blocking connect: bounded wait for the handshake (startup only, not the trading loop).
    pollfd entry{fd, POLLOUT, 0};
    if (::poll(&entry, 1, timeout_ms) != 1) {
        error = "connect to " + host + ":" + port_text + ": timed out";
        ::close(fd);
        return -1;
    }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
    if (socket_error != 0) {
        error = "connect to " + host + ":" + port_text + ": " + std::strerror(socket_error);
        ::close(fd);
        return -1;
    }
    return fd;
}

void TcpConnection::write(std::string_view bytes) {
    if (fd_ < 0) return;
    if (pending_output_.empty()) {
        // Nothing queued: send straight from `bytes` without copying.
        bytes.remove_prefix(send_now(bytes));
        if (bytes.empty() || fd_ < 0) return;
    }
    // Socket full or older bytes still queued: append behind them to keep wire order.
    pending_output_.append(bytes);
    if (pending_output_.size() > kMaxPendingOutput) close();
}

void TcpConnection::flush() {
    if (fd_ < 0 || pending_output_.empty()) return;
    const size_t sent = send_now(pending_output_);
    pending_output_.erase(0, sent);
}

size_t TcpConnection::send_now(std::string_view bytes) {
    size_t total = 0;
    while (total < bytes.size()) {
        const ssize_t sent = ::send(fd_, bytes.data() + total, bytes.size() - total, kSendFlags);
        if (sent > 0) {
            total += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // kernel buffer full
        close();  // EPIPE, ECONNRESET: the connection is gone
        break;
    }
    return total;
}

TcpConnection::ReadResult TcpConnection::read_some(char* buffer, size_t capacity) {
    if (fd_ < 0) return {ReadStatus::Closed, 0};
    const ssize_t received = ::recv(fd_, buffer, capacity, 0);
    if (received > 0) return {ReadStatus::Data, static_cast<size_t>(received)};
    if (received == 0) return {ReadStatus::Closed, 0};  // orderly shutdown by the peer
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {ReadStatus::WouldBlock, 0};
    return {ReadStatus::Closed, 0};  // ECONNRESET and friends
}

void TcpConnection::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    pending_output_.clear();
}

}  // namespace mt::fix
