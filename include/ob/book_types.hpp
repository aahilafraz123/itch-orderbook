// Types shared by every order book implementation (reference and optimized),
// so the differential test can compare their outputs directly.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ob/itch_messages.hpp"

namespace ob {

using itch::Side;
using OrderRef = std::uint64_t;
using Price    = std::uint32_t;  // 4 implied decimals, never floating point
using Shares   = std::uint32_t;
using Locate   = std::uint16_t;  // per-day stock ID, 0..65535

inline constexpr std::size_t kMaxLocates = std::size_t{1} << 16;

// One price level as seen from outside the book.
struct LevelSummary {
    Price         price;
    std::uint64_t shares;  // sum over the level's orders; 64-bit so it can't overflow
    std::uint32_t orders;

    bool operator==(const LevelSummary&) const = default;
};

// Messages that don't make sense against the current book. On a full-day file
// replayed from the start, every counter should stay 0; if one doesn't, either
// the data or our logic is wrong. Policy: count it, leave the book unchanged
// (except over_reduce, which removes the order since it can't have shares left).
// Every book implementation must apply exactly this policy.
struct BookAnomalies {
    std::uint64_t unknown_ref    = 0;  // E/C/X/D/U for an order ref we don't have
    std::uint64_t duplicate_ref  = 0;  // A/F/U adding a ref that's already live
    std::uint64_t over_reduce    = 0;  // E/C/X taking more shares than remain
    std::uint64_t zero_share_add = 0;  // A/F/U adding an order with 0 shares
    std::uint64_t bad_side       = 0;  // side byte not 'B' or 'S'

    std::uint64_t total() const {
        return unknown_ref + duplicate_ref + over_reduce + zero_share_add + bad_side;
    }
    bool operator==(const BookAnomalies&) const = default;
};

}  // namespace ob
