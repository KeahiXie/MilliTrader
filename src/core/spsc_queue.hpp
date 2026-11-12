#pragma once
// SpscQueue<T>: bounded lock-free ring for exactly one producer and one consumer thread.
// Power-of-two capacity; head/tail count up forever and are masked only when indexing.
// Storage is allocated once in the constructor; push/pop never allocate, block or syscall.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mt {

// 64 bytes on x86; Apple M-series uses 128-byte lines, so pad to 128 on aarch64.
#if defined(__aarch64__)
inline constexpr std::size_t kCacheLineSize = 128;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

template <typename T>
class SpscQueue {
    static_assert(std::is_default_constructible_v<T>, "slots are constructed up front");

public:
    explicit SpscQueue(std::size_t capacity)
        : capacity_(capacity), mask_(capacity - 1), slots_(std::make_unique<T[]>(capacity)) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("SpscQueue capacity must be a power of two >= 2");
        }
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer thread only. Returns false if full.
    bool try_push(const T& item) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only we write tail_
        if (tail - producer_cached_head_ == capacity_) {  // looks full: refresh cached head
            producer_cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - producer_cached_head_ == capacity_) return false;  // really full
        }
        slots_[tail & mask_] = item;
        // release: consumer must see the slot write before the new tail
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& item) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - producer_cached_head_ == capacity_) {
            producer_cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - producer_cached_head_ == capacity_) return false;
        }
        slots_[tail & mask_] = std::move(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns false if empty.
    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);  // only we write head_
        if (head == consumer_cached_tail_) {  // looks empty: refresh cached tail
            consumer_cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == consumer_cached_tail_) return false;  // really empty
        }
        out = std::move(slots_[head & mask_]);
        // release: producer may reuse the slot only after we've read it
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate; stats only.
    std::size_t size_approx() const {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    std::size_t capacity() const { return capacity_; }

private:
    // Read-only after construction, so sharing these is free.
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> slots_;

    // Separate cache lines so producer and consumer don't false-share.
    // Consumer line: head plus its cached copy of tail.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    std::size_t consumer_cached_tail_ = 0;

    // Producer line: tail plus its cached copy of head.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    std::size_t producer_cached_head_ = 0;
    // alignas rounds sizeof up to a whole line, so no trailing padding needed.
};

}  // namespace mt
