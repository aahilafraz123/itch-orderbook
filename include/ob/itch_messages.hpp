// ITCH 5.0 wire format: big-endian field readers and decoded message structs.
//
// Offsets and lengths are taken from the NASDAQ TotalView-ITCH 5.0 specification,
// sections 1.3 (Add Order 'A', 'F') and 1.4 (Modify Order 'E', 'C', 'X', 'D', 'U').
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ob::itch {

// The byte-swap readers below assume the CPU stores integers little-endian
// (true for x86-64 and Apple Silicon / ARM64 Linux). Fail the build otherwise.
static_assert(std::endian::native == std::endian::little,
              "big-endian readers assume a little-endian host");

// ---------------------------------------------------------------------------
// Big-endian readers.
//
// We copy bytes into a local integer with memcpy instead of doing
// `*reinterpret_cast<const uint32_t*>(p)`. The cast is undefined behavior
// (misaligned access + strict aliasing); memcpy is defined, and compilers turn a
// fixed-size memcpy into a single load instruction, so it costs nothing.
// The swap then converts network order (big-endian) to host order.
// ---------------------------------------------------------------------------
inline std::uint16_t read_u16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap16(v);
}

inline std::uint32_t read_u32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);
}

inline std::uint64_t read_u64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap64(v);
}

// ITCH timestamps are 6 bytes: the high 2 bytes followed by the low 4 bytes.
inline std::uint64_t read_u48(const std::uint8_t* p) noexcept {
    return (std::uint64_t{read_u16(p)} << 32) | read_u32(p + 2);
}

// ---------------------------------------------------------------------------
// Decoded messages.
//
// These are plain structs holding host-order values, not packed structs laid
// over the raw bytes. A packed overlay would still need every field byte-swapped
// and would make the compiler emit misaligned accesses.
// ---------------------------------------------------------------------------

// Every ITCH message begins with these 11 bytes.
struct Header {
    char          type;        // offset 0
    std::uint16_t locate;      // offset 1  per-day stock ID, used as the book index
    std::uint16_t tracking;    // offset 3  NASDAQ internal, unused by the book
    std::uint64_t timestamp;   // offset 5  nanoseconds since midnight (6 bytes)
};
inline constexpr std::size_t kHeaderLen = 11;

enum class Side : char { Buy = 'B', Sell = 'S' };

// Alpha fields are ASCII, right-padded with spaces. Return the text without the padding.
template <std::size_t N>
inline std::string_view trim_alpha(const std::array<char, N>& a) noexcept {
    std::string_view s(a.data(), N);
    return s.substr(0, s.find_last_not_of(' ') + 1);
}

// 'A' Add Order - No MPID Attribution (36 bytes) and
// 'F' Add Order - MPID Attribution (40 bytes: the same 36, plus attribution).
//
// Both decode into this one struct. The book treats them identically (who
// posted an order doesn't change where it sits), so one type means one code
// path to test. header.type still says which one arrived.
struct AddOrder {
    Header               header;
    std::uint64_t        order_ref;    // offset 11
    Side                 side;         // offset 19
    std::uint32_t        shares;       // offset 20
    std::array<char, 8>  stock;        // offset 24  e.g. "AAPL    "
    std::uint32_t        price;        // offset 32  4 implied decimals: 1900500 = $190.0500
    std::array<char, 4>  attribution;  // offset 36  'F' only: market participant ID, e.g. "GSCO".
                                       //            All spaces for 'A'.

    std::string_view symbol() const noexcept { return trim_alpha(stock); }
    std::string_view mpid() const noexcept { return trim_alpha(attribution); }
};
inline constexpr std::size_t kAddOrderLen     = 36;
inline constexpr std::size_t kAddOrderMpidLen = 40;

// 'E' Order Executed. 31 bytes.
// Some of the order's shares traded at the order's own price.
// Executions are cumulative: subtract from the order's remaining shares.
struct OrderExecuted {
    Header        header;
    std::uint64_t order_ref;        // offset 11
    std::uint32_t executed_shares;  // offset 19
    std::uint64_t match_number;     // offset 23  day-unique trade ID
};
inline constexpr std::size_t kOrderExecutedLen = 31;

// 'C' Order Executed With Price. 36 bytes.
// Same as 'E', but the trade happened at a price different from the order's
// display price (e.g. during a cross). The order's resting price doesn't change;
// the book still just subtracts executed_shares.
struct OrderExecutedWithPrice {
    Header        header;
    std::uint64_t order_ref;        // offset 11
    std::uint32_t executed_shares;  // offset 19
    std::uint64_t match_number;     // offset 23
    char          printable;        // offset 31  'Y' or 'N'. 'N' = counted in a later bulk print
    std::uint32_t execution_price;  // offset 32  4 implied decimals
};
inline constexpr std::size_t kOrderExecutedWithPriceLen = 36;

// 'X' Order Cancel. 23 bytes.
// Partial cancel: the owner pulled some shares. Subtract from remaining shares.
struct OrderCancel {
    Header        header;
    std::uint64_t order_ref;        // offset 11
    std::uint32_t canceled_shares;  // offset 19
};
inline constexpr std::size_t kOrderCancelLen = 23;

// 'D' Order Delete. 19 bytes.
// The whole order is gone, whatever shares were left.
struct OrderDelete {
    Header        header;
    std::uint64_t order_ref;  // offset 11
};
inline constexpr std::size_t kOrderDeleteLen = 19;

// 'U' Order Replace. 35 bytes.
// Cancel-replace: remove the original order, then add a new one under a new
// reference number. Side and stock aren't in the message because they can't
// change; the book copies them from the original order. The new order goes to
// the back of the queue at its price (it loses time priority).
struct OrderReplace {
    Header        header;
    std::uint64_t original_order_ref;  // offset 11
    std::uint64_t new_order_ref;       // offset 19  used by all later messages
    std::uint32_t shares;              // offset 27  new total displayed shares
    std::uint32_t price;               // offset 31  new display price
};
inline constexpr std::size_t kOrderReplaceLen = 35;

// ---------------------------------------------------------------------------
// Decoders. Caller guarantees `p` points at a full message of the right length.
// ---------------------------------------------------------------------------
inline Header decode_header(const std::uint8_t* p) noexcept {
    return Header{
        static_cast<char>(p[0]),
        read_u16(p + 1),
        read_u16(p + 3),
        read_u48(p + 5),
    };
}

inline AddOrder decode_add_order(const std::uint8_t* p) noexcept {
    AddOrder m;
    m.header    = decode_header(p);
    m.order_ref = read_u64(p + 11);
    m.side      = static_cast<Side>(p[19]);
    m.shares    = read_u32(p + 20);
    std::memcpy(m.stock.data(), p + 24, m.stock.size());
    m.price     = read_u32(p + 32);
    m.attribution.fill(' ');
    return m;
}

inline AddOrder decode_add_order_mpid(const std::uint8_t* p) noexcept {
    AddOrder m = decode_add_order(p);
    std::memcpy(m.attribution.data(), p + 36, m.attribution.size());
    return m;
}

inline OrderExecuted decode_order_executed(const std::uint8_t* p) noexcept {
    return OrderExecuted{decode_header(p), read_u64(p + 11), read_u32(p + 19), read_u64(p + 23)};
}

inline OrderExecutedWithPrice decode_order_executed_with_price(const std::uint8_t* p) noexcept {
    return OrderExecutedWithPrice{
        decode_header(p),
        read_u64(p + 11),
        read_u32(p + 19),
        read_u64(p + 23),
        static_cast<char>(p[31]),
        read_u32(p + 32),
    };
}

inline OrderCancel decode_order_cancel(const std::uint8_t* p) noexcept {
    return OrderCancel{decode_header(p), read_u64(p + 11), read_u32(p + 19)};
}

inline OrderDelete decode_order_delete(const std::uint8_t* p) noexcept {
    return OrderDelete{decode_header(p), read_u64(p + 11)};
}

inline OrderReplace decode_order_replace(const std::uint8_t* p) noexcept {
    return OrderReplace{decode_header(p), read_u64(p + 11), read_u64(p + 19),
                        read_u32(p + 27), read_u32(p + 31)};
}

}  // namespace ob::itch
