#include "fix/sequence_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace mt::fix {

SequenceStore::SequenceStore(std::string path) : path_(std::move(path)) {
    if (path_.empty()) return;
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("cannot open FIX sequence file " + path_ + ": " + std::strerror(errno));
    }
}

SequenceStore::~SequenceStore() {
    if (fd_ >= 0) ::close(fd_);
}

bool SequenceStore::load(uint64_t& next_outgoing, uint64_t& next_incoming) const {
    if (fd_ < 0) return false;
    char text[128] = {};
    const ssize_t length = ::pread(fd_, text, sizeof(text) - 1, 0);
    if (length <= 0) return false;  // new, empty file

    unsigned long long saved_outgoing = 0;
    unsigned long long saved_incoming = 0;
    if (std::sscanf(text, "next_out=%llu next_in=%llu", &saved_outgoing, &saved_incoming) != 2) return false;
    if (saved_outgoing == 0 || saved_incoming == 0) return false;
    next_outgoing = saved_outgoing;
    next_incoming = saved_incoming;
    return true;
}

bool SequenceStore::save(uint64_t next_outgoing, uint64_t next_incoming) {
    if (fd_ < 0) return true;
    char text[80];
    const int length = std::snprintf(text, sizeof(text), "next_out=%020llu next_in=%020llu\n",
                                     static_cast<unsigned long long>(next_outgoing),
                                     static_cast<unsigned long long>(next_incoming));
    return ::pwrite(fd_, text, static_cast<size_t>(length), 0) == length;
}

}  // namespace mt::fix
