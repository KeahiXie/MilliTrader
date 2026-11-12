#pragma once
// ControlChannel: sends the engine a Timer event every 100 ms (drives order timeouts) and forwards
// operator commands from a Unix datagram socket, both as ControlEvents on an SPSC queue:
//   kill | resume | max_order_qty <contracts> | max_position <contracts> | shutdown

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "core/events.hpp"
#include "core/spsc_queue.hpp"

namespace mt {

using ControlQueue = SpscQueue<ControlEvent>;

// Returns false for an unknown command.
bool parse_control_command(const std::string& line, int64_t now_ns, ControlEvent* event);

class ControlChannel {
public:
    explicit ControlChannel(ControlQueue& out, std::string socket_path);
    ~ControlChannel();

    void start();
    void stop();
    void request_shutdown();  // thread-safe, e.g. from a signal handler path

private:
    void run();
    void push(const ControlEvent& event);

    ControlQueue& out_;
    std::string socket_path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    int socket_fd_ = -1;
};

}  // namespace mt
