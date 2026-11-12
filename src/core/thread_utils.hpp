#pragma once
// Start named threads pinned to a CPU core. Pinning keeps caches warm (pair with
// isolcpus/nohz_full to cut jitter); names show up in top -H, perf and gdb.

#include <pthread.h>

#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace mt {

// Pin the calling thread to one core. Returns false if the core is missing or not allowed.
inline bool pin_current_thread(int core) {
#if defined(__linux__)
    if (core < 0) return true;  // -1 means "do not pin"
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(core, &cpus);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) == 0;
#else
    (void)core;
    return false;
#endif
}

inline void name_current_thread(const std::string& name) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());  // kernel limit: 15 chars
#elif defined(__APPLE__)
    pthread_setname_np(name.c_str());
#endif
}

// Start `body` on a new thread that names and pins itself first.
template <typename Function>
std::thread start_thread(std::string name, int core, Function&& body) {
    return std::thread([name = std::move(name), core, body = std::forward<Function>(body)]() mutable {
        name_current_thread(name);
        if (core >= 0 && !pin_current_thread(core)) {
            std::fprintf(stderr, "warning: could not pin thread %s to core %d\n", name.c_str(), core);
        }
        body();
    });
}

}  // namespace mt
