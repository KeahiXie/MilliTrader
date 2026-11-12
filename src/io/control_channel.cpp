#include "io/control_channel.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/clock.hpp"
#include "core/thread_utils.hpp"

namespace mt {

namespace {
constexpr int64_t kTimerPeriod = 100 * kNanosPerMilli;
}

bool parse_control_command(const std::string& line, int64_t now_ns, ControlEvent* event) {
    std::istringstream words(line);
    std::string command;
    words >> command;
    event->now_ns = now_ns;
    event->value = 0;
    if (command == "kill") {
        event->type = ControlEventType::KillSwitchOn;
    } else if (command == "resume") {
        event->type = ControlEventType::KillSwitchOff;
    } else if (command == "shutdown") {
        event->type = ControlEventType::Shutdown;
    } else if (command == "max_order_qty" || command == "max_position") {
        double contracts_value = 0;
        if (!(words >> contracts_value) || contracts_value < 0) return false;
        event->type = command == "max_order_qty" ? ControlEventType::SetMaxOrderQty : ControlEventType::SetMaxPosition;
        event->value = static_cast<int64_t>(contracts_value * kQtyPerContract);
    } else {
        return false;
    }
    return true;
}

ControlChannel::ControlChannel(ControlQueue& out, std::string socket_path)
    : out_(out), socket_path_(std::move(socket_path)) {}

ControlChannel::~ControlChannel() { stop(); }

void ControlChannel::start() {
    socket_fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
    unlink(socket_path_.c_str());
    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "control: cannot bind %s (commands disabled)\n", socket_path_.c_str());
        close(socket_fd_);
        socket_fd_ = -1;
    }
    running_ = true;
    thread_ = start_thread("mt-control", -1, [this] { run(); });
}

void ControlChannel::stop() {
    if (!running_.exchange(false)) return;
    thread_.join();
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        unlink(socket_path_.c_str());
    }
}

void ControlChannel::request_shutdown() { shutdown_requested_ = true; }

void ControlChannel::push(const ControlEvent& event) {
    ControlEvent stamped = event;
    stamped.enqueue_mono_ns = monotonic_ns();
    while (!out_.try_push(stamped)) cpu_relax();
}

void ControlChannel::run() {
    int64_t next_timer = wall_clock_ns();
    char buffer[256];
    bool shutdown_sent = false;
    while (running_) {
        const int64_t now = wall_clock_ns();
        if (now >= next_timer) {
            ControlEvent timer;
            timer.type = ControlEventType::Timer;
            timer.now_ns = now;
            push(timer);
            next_timer += kTimerPeriod;
            if (next_timer < now) next_timer = now + kTimerPeriod;  // we were descheduled: don't burst
        }
        if (shutdown_requested_ && !shutdown_sent) {
            ControlEvent shutdown;
            shutdown.type = ControlEventType::Shutdown;
            shutdown.now_ns = now;
            push(shutdown);
            shutdown_sent = true;
        }

        // Wait for a command until the next timer tick is due.
        const int wait_ms = static_cast<int>(std::max<int64_t>(0, (next_timer - wall_clock_ns()) / kNanosPerMilli));
        if (socket_fd_ < 0) {
            usleep(static_cast<useconds_t>(wait_ms) * 1000);
            continue;
        }
        pollfd waiting{socket_fd_, POLLIN, 0};
        if (poll(&waiting, 1, wait_ms) > 0 && (waiting.revents & POLLIN)) {
            const ssize_t length = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
            if (length <= 0) continue;
            buffer[length] = '\0';
            ControlEvent event;
            if (parse_control_command(buffer, wall_clock_ns(), &event)) {
                std::fprintf(stderr, "control: %s\n", buffer);
                push(event);
            } else {
                std::fprintf(stderr, "control: unknown command '%s'\n", buffer);
            }
        }
    }
}

}  // namespace mt
