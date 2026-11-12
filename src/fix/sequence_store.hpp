#pragma once
// SequenceStore: persists a FIX session's next outgoing/incoming MsgSeqNum so a restart resumes them
// (the peer remembers them and would reject a Logon that restarts at 1). One fixed-width line, rewritten
// with a single pwrite() (no truncate needed). No fsync: survives a process crash, not a power cut.

#include <cstdint>
#include <string>

namespace mt::fix {

class SequenceStore {
public:
    // Empty path = memory only. Throws if the file can't be opened: running with unpersisted
    // sequence numbers is worse than refusing to start.
    explicit SequenceStore(std::string path);
    ~SequenceStore();
    SequenceStore(const SequenceStore&) = delete;
    SequenceStore& operator=(const SequenceStore&) = delete;

    // Returns false and leaves the arguments alone if there is no file yet or it is unreadable.
    bool load(uint64_t& next_outgoing, uint64_t& next_incoming) const;
    // Returns false if the write failed.
    bool save(uint64_t next_outgoing, uint64_t next_incoming);

    bool is_persistent() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    int fd_ = -1;
};

}  // namespace mt::fix
