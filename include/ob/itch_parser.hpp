// Walks a buffer of length-prefixed ITCH messages and hands each one to a handler.
//
// File framing (NASDAQ sample files): [2-byte big-endian length][message bytes]...
// The first message byte is its type, so we can skip any type we don't decode
// just by jumping `length` bytes ahead.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "ob/itch_messages.hpp"

namespace ob::itch {

// What a handler must provide. Checked at compile time, so a typo in a handler
// method name is a clear error instead of a page of template noise.
template <class H>
concept Handler = requires(H& h, const AddOrder& a, const OrderDelete& d,
                           char type, const std::uint8_t* bytes, std::size_t len) {
    h.on_add_order(a);
    h.on_order_delete(d);
    h.on_other(type, bytes, len);  // every type we don't decode yet
};

// Decode one message (without its length prefix) and call the handler.
// Returns false if the length doesn't match what the spec says for that type,
// which means the stream is corrupt or our framing is off.
//
// The handler is a template parameter, not a virtual interface: the compiler
// knows the exact handler type, so it can inline on_add_order() straight into
// this switch. A virtual call is an indirect jump through a vtable that the
// optimizer usually can't see through, and this runs hundreds of millions of times.
template <Handler H>
inline bool dispatch(const std::uint8_t* msg, std::size_t len, H& handler) {
    const char type = static_cast<char>(msg[0]);
    switch (type) {
        case 'A':
            if (len != kAddOrderLen) return false;
            handler.on_add_order(decode_add_order(msg));
            return true;
        case 'D':
            if (len != kOrderDeleteLen) return false;
            handler.on_order_delete(decode_order_delete(msg));
            return true;
        default:
            handler.on_other(type, msg, len);
            return true;
    }
}

enum class ParseStatus {
    Ok,         // reached the end of the buffer, or max_messages
    Truncated,  // buffer ends partway through a message (normal for a file prefix)
    BadLength,  // a length field is 0 or doesn't match the message type
};

struct ParseResult {
    ParseStatus   status;
    std::uint64_t messages;  // messages successfully dispatched
    std::size_t   offset;    // byte offset where parsing stopped
};

// Parse up to `max_messages` framed messages from `buf`.
template <Handler H>
ParseResult parse(std::span<const std::uint8_t> buf, H& handler,
                  std::uint64_t max_messages = std::numeric_limits<std::uint64_t>::max()) {
    const std::uint8_t* data = buf.data();
    const std::size_t   size = buf.size();
    std::size_t   pos = 0;
    std::uint64_t n   = 0;

    while (n < max_messages && pos < size) {
        if (size - pos < 2) return {ParseStatus::Truncated, n, pos};

        const std::size_t len = read_u16(data + pos);
        if (len == 0) return {ParseStatus::BadLength, n, pos};
        if (size - pos - 2 < len) return {ParseStatus::Truncated, n, pos};

        if (!dispatch(data + pos + 2, len, handler)) return {ParseStatus::BadLength, n, pos};

        pos += 2 + len;
        ++n;
    }
    return {ParseStatus::Ok, n, pos};
}

}  // namespace ob::itch
