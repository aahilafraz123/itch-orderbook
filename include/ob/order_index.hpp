// Order-ref -> pool-slot lookup tables for the optimized book.
//
// Every E/C/X/D/U message carries only an order ref, so this lookup runs on
// almost every message. Two interchangeable implementations with the same
// interface, so the benchmark can measure one against the other:
//
//   StdIndex             std::unordered_map. Node-based: every insert
//                        allocates a node, every lookup chases a pointer to a
//                        node somewhere on the heap (likely a cache miss).
//   OpenAddressingIndex  one flat array of {key, value} slots, linear probing.
//                        No allocation after construction (until it must
//                        grow), and a lookup usually touches one cache line.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace ob {

inline constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();

class StdIndex {
public:
    explicit StdIndex(std::size_t expected) { map_.reserve(expected); }

    std::uint32_t find(std::uint64_t key) const {
        auto it = map_.find(key);
        return it == map_.end() ? kNoSlot : it->second;
    }
    void insert(std::uint64_t key, std::uint32_t value) { map_.emplace(key, value); }
    void erase(std::uint64_t key) { map_.erase(key); }
    std::size_t size() const { return map_.size(); }

private:
    std::unordered_map<std::uint64_t, std::uint32_t> map_;
};

// Linear-probing hash table with backward-shift deletion.
//
// - Capacity is a power of two, kept at most half full, so probe sequences
//   stay short. It doubles (one rehash, one allocation) if that's exceeded.
// - Hash: Fibonacci hashing (multiply by 2^64/phi, keep the top bits). NASDAQ
//   order refs are close to sequential; the multiply spreads neighbors across
//   the table so they don't pile into one long run.
// - Delete: instead of leaving "tombstones" (which slowly fill the table and
//   lengthen probes over a day of ~hundreds of millions of deletes), later
//   entries in the same run are shifted back into the hole.
class OpenAddressingIndex {
public:
    explicit OpenAddressingIndex(std::size_t expected) {
        allocate(std::bit_ceil(std::max<std::size_t>(16, expected * 2)));
    }

    std::uint32_t find(std::uint64_t key) const {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == kNoSlot) return kNoSlot;
            if (s.key == key) return s.value;
        }
    }

    // Caller guarantees `key` isn't already present.
    void insert(std::uint64_t key, std::uint32_t value) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        place(key, value);
        ++size_;
    }

    void erase(std::uint64_t key) {
        std::size_t hole = home(key);
        while (slots_[hole].key != key || slots_[hole].value == kNoSlot) {
            if (slots_[hole].value == kNoSlot) return;  // not present
            hole = (hole + 1) & mask_;
        }
        // Walk the rest of the run. Any entry whose home slot is not
        // cyclically in (hole, j] can legally move back into the hole.
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != kNoSlot; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            const bool movable = hole <= j ? (h <= hole || h > j) : (h <= hole && h > j);
            if (movable) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = kNoSlot;
        --size_;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

private:
    struct Slot {
        std::uint64_t key;
        std::uint32_t value;  // kNoSlot marks an empty slot (refs can be any value)
    };

    std::size_t home(std::uint64_t key) const {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void allocate(std::size_t capacity) {
        slots_.assign(capacity, Slot{0, kNoSlot});
        mask_  = capacity - 1;
        shift_ = 64 - std::countr_zero(capacity);
    }

    void place(std::uint64_t key, std::uint32_t value) {
        std::size_t i = home(key);
        while (slots_[i].value != kNoSlot) i = (i + 1) & mask_;
        slots_[i] = Slot{key, value};
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        allocate(old.size() * 2);
        for (const Slot& s : old) {
            if (s.value != kNoSlot) place(s.key, s.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t       mask_  = 0;
    int               shift_ = 0;
    std::size_t       size_  = 0;
};

}  // namespace ob
