#pragma once
// FixedHashSet: fixed-capacity set of 64-bit ids for duplicate detection on the engine thread.
// One array with linear probing, allocated once, so insert never mallocs. No erase.

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mt {

class FixedHashSet {
public:
    explicit FixedHashSet(size_t capacity_pow2) : slots_(capacity_pow2, kEmpty), mask_(capacity_pow2 - 1) {
        if (capacity_pow2 < 2 || (capacity_pow2 & mask_) != 0) {
            throw std::invalid_argument("FixedHashSet capacity must be a power of two");
        }
    }

    // Returns false if already present. Throws past 3/4 full so long probes fail
    // loudly instead of quietly slowing the engine.
    bool insert(uint64_t value) {
        if (value == kEmpty) value = kEmptyReplacement;  // 0 marks a free slot
        size_t index = mix(value) & mask_;
        while (slots_[index] != kEmpty) {
            if (slots_[index] == value) return false;
            index = (index + 1) & mask_;
        }
        if (++size_ * 4 > slots_.size() * 3) throw std::length_error("FixedHashSet is too full");
        slots_[index] = value;
        return true;
    }

    bool contains(uint64_t value) const {
        if (value == kEmpty) value = kEmptyReplacement;
        size_t index = mix(value) & mask_;
        while (slots_[index] != kEmpty) {
            if (slots_[index] == value) return true;
            index = (index + 1) & mask_;
        }
        return false;
    }

    size_t size() const { return size_; }

private:
    static constexpr uint64_t kEmpty = 0;
    static constexpr uint64_t kEmptyReplacement = 0x9E3779B97F4A7C15ULL;

    // splitmix64 finalizer: spreads sequential ids across the table.
    static uint64_t mix(uint64_t x) {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    std::vector<uint64_t> slots_;
    size_t mask_;
    size_t size_ = 0;
};

// 64-bit FNV-1a; turns a venue fill id string into GatewayEvent::fill_id.
inline uint64_t hash_string(const char* data, size_t length) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace mt
