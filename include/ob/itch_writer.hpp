// Encodes ITCH 5.0 messages into framed bytes ([2-byte length][message]).
//
// Used only to generate synthetic test streams. The parser's own unit tests
// deliberately use hand-written byte literals instead, so a misunderstanding
// of the format can't hide by being shared between encoder and decoder.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "ob/book_types.hpp"

namespace ob::itch {

class Writer {
public:
    std::vector<std::uint8_t>&       bytes() { return out_; }
    const std::vector<std::uint8_t>& bytes() const { return out_; }
    std::uint64_t                    messages() const { return messages_; }

    void add(Locate loc, OrderRef ref, Side side, Shares shares, std::string_view sym, Price price,
             std::string_view mpid = {}) {
        const bool attributed = !mpid.empty();
        begin(attributed ? 40 : 36, attributed ? 'F' : 'A', loc);
        u64(ref);
        out_.push_back(static_cast<std::uint8_t>(side));
        u32(shares);
        alpha(sym, 8);
        u32(price);
        if (attributed) alpha(mpid, 4);
    }

    void executed(Locate loc, OrderRef ref, Shares n, std::uint64_t match) {
        begin(31, 'E', loc);
        u64(ref); u32(n); u64(match);
    }

    void executed_with_price(Locate loc, OrderRef ref, Shares n, std::uint64_t match, char printable, Price px) {
        begin(36, 'C', loc);
        u64(ref); u32(n); u64(match);
        out_.push_back(static_cast<std::uint8_t>(printable));
        u32(px);
    }

    void cancel(Locate loc, OrderRef ref, Shares n) {
        begin(23, 'X', loc);
        u64(ref); u32(n);
    }

    void del(Locate loc, OrderRef ref) {
        begin(19, 'D', loc);
        u64(ref);
    }

    void replace(Locate loc, OrderRef orig, OrderRef new_ref, Shares shares, Price price) {
        begin(35, 'U', loc);
        u64(orig); u64(new_ref); u32(shares); u32(price);
    }

    void system_event(char code) {
        begin(12, 'S', 0);
        out_.push_back(static_cast<std::uint8_t>(code));
    }

private:
    void begin(std::uint16_t len, char type, Locate loc) {
        u16(len);
        out_.push_back(static_cast<std::uint8_t>(type));
        u16(loc);
        u16(0);                 // tracking number
        u48(++timestamp_);      // strictly increasing, 1 ns apart
        ++messages_;
    }

    void u16(std::uint16_t v) { put(v, 2); }
    void u32(std::uint32_t v) { put(v, 4); }
    void u48(std::uint64_t v) { put(v, 6); }
    void u64(std::uint64_t v) { put(v, 8); }

    void put(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void alpha(std::string_view s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) out_.push_back(static_cast<std::uint8_t>(i < s.size() ? s[i] : ' '));
    }

    std::vector<std::uint8_t> out_;
    std::uint64_t             timestamp_ = 34'200'000'000'000ull;  // 09:30:00
    std::uint64_t             messages_  = 0;
};

}  // namespace ob::itch
