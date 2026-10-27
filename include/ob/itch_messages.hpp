// ITCH 5.0 wire format: big-endian field readers and decoded message structs.
//
// Offsets and lengths are taken from the NASDAQ TotalView-ITCH 5.0 specification
// (sections 1.3.1 Add Order - No MPID Attribution, 1.4.4 Order Delete).
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

// 'A' Add Order - No MPID Attribution. 36 bytes.
struct AddOrder {
    Header               header;
    std::uint64_t        order_ref;  // offset 11
    Side                 side;       // offset 19
    std::uint32_t        shares;     // offset 20
    std::array<char, 8>  stock;      // offset 24  ASCII, right-padded with spaces
    std::uint32_t        price;      // offset 32  4 implied decimals: 1900500 = $190.0500

    // Symbol without the trailing spaces, e.g. "AAPL".
    std::string_view symbol() const noexcept {
        std::string_view s(stock.data(), stock.size());
        return s.substr(0, s.find_last_not_of(' ') + 1);
    }
};
inline constexpr std::size_t kAddOrderLen = 36;

// 'D' Order Delete. 19 bytes.
struct OrderDelete {
    Header        header;
    std::uint64_t order_ref;  // offset 11
};
inline constexpr std::size_t kOrderDeleteLen = 19;

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
    return m;
}

inline OrderDelete decode_order_delete(const std::uint8_t* p) noexcept {
    return OrderDelete{decode_header(p), read_u64(p + 11)};
}

}  // namespace ob::itch
