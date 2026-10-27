// Lock-free single-producer / single-consumer ring buffer.
//
// Exactly one thread calls try_push and exactly one other thread calls try_pop.
// With only one writer per index, no locks or compare-and-swap are needed:
//
//   tail_  written only by the producer: "slots [head, tail) hold data"
//   head_  written only by the consumer: "slots before head are free again"
//
// Memory ordering:
//   - The producer writes the element, THEN publishes tail_ with a release
//     store. The consumer reads tail_ with an acquire load, so if it sees the
//     new tail it is guaranteed to also see the element written before it.
//   - Symmetrically, the consumer releases head_ after it has finished reading
//     a slot, and the producer acquires head_ before overwriting that slot.
//   - Each side reads its OWN index with a relaxed load (nobody else writes it).
//
// False sharing: head_ and tail_ are written by different cores. If they sat in
// the same cache line, every write by one core would invalidate the line in the
// other core's cache, and the two would ping-pong it constantly. So each index
// lives in its own cache-line-aligned block, together with the private cached
// copy of the other side's index that only its owner touches.
//
// Cached indices: the producer only re-reads the consumer's head_ (a cross-core
// cache miss) when its cached copy says the ring looks full; likewise for the
// consumer and tail_. In the common case each operation touches only lines its
// own core already owns.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>

namespace ob {

// 128 covers Apple Silicon (128-byte lines) and x86 (64-byte lines, but the
// adjacent-line prefetcher pulls pairs of lines).
inline constexpr std::size_t kCacheLine = 128;

template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity)
        : capacity_(std::bit_ceil(capacity)), mask_(capacity_ - 1), buf_(std::make_unique<T[]>(capacity_)) {}

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer thread only.
    bool try_push(const T& value) {
        const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
        if (tail - prod_.cached_head == capacity_) {
            prod_.cached_head = cons_.head.load(std::memory_order_acquire);
            if (tail - prod_.cached_head == capacity_) return false;  // really full
        }
        buf_[tail & mask_] = value;
        prod_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) {
        const std::size_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.cached_tail) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            if (head == cons_.cached_tail) return false;  // really empty
        }
        out = buf_[head & mask_];
        cons_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const { return capacity_; }

private:
    struct alignas(kCacheLine) ProducerSide {
        std::atomic<std::size_t> tail{0};
        std::size_t              cached_head = 0;
    };
    struct alignas(kCacheLine) ConsumerSide {
        std::atomic<std::size_t> head{0};
        std::size_t              cached_tail = 0;
    };

    ProducerSide         prod_;
    ConsumerSide         cons_;
    const std::size_t    capacity_;
    const std::size_t    mask_;
    std::unique_ptr<T[]> buf_;
};

}  // namespace ob
