// itch_stats: replay an ITCH 5.0 file and print how many messages of each type it has.
//
// Usage: itch_stats <file> [max_messages] [--book SYMBOL]
//   file           decompressed NASDAQ ITCH 5.0 file (e.g. 12302019.NASDAQ_ITCH50)
//   max_messages   stop after this many messages (handy for a quick look at a huge file)
//   --book SYMBOL  also rebuild the order book with the reference book and print
//                  SYMBOL's top levels, book-wide anomaly counts and a consistency check

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include "ob/itch_parser.hpp"
#include "ob/mapped_file.hpp"
#include "ob/reference_book.hpp"

namespace {

// Counts every message by type and, if a book is attached, forwards the
// book-changing ones to it.
struct CountingHandler {
    std::array<std::uint64_t, 256> counts{};
    ob::ReferenceBook*             book = nullptr;

    void count(char type) { ++counts[static_cast<unsigned char>(type)]; }

    void on_add_order(const ob::itch::AddOrder& m) {
        count(m.header.type);
        if (book) book->on_add_order(m);
    }
    void on_order_executed(const ob::itch::OrderExecuted& m) {
        count(m.header.type);
        if (book) book->on_order_executed(m);
    }
    void on_order_executed_with_price(const ob::itch::OrderExecutedWithPrice& m) {
        count(m.header.type);
        if (book) book->on_order_executed_with_price(m);
    }
    void on_order_cancel(const ob::itch::OrderCancel& m) {
        count(m.header.type);
        if (book) book->on_order_cancel(m);
    }
    void on_order_delete(const ob::itch::OrderDelete& m) {
        count(m.header.type);
        if (book) book->on_order_delete(m);
    }
    void on_order_replace(const ob::itch::OrderReplace& m) {
        count(m.header.type);
        if (book) book->on_order_replace(m);
    }
    void on_other(char type, const std::uint8_t*, std::size_t) { count(type); }
};

const char* status_name(ob::itch::ParseStatus s) {
    switch (s) {
        case ob::itch::ParseStatus::Ok:        return "ok";
        case ob::itch::ParseStatus::Truncated: return "truncated (file ends mid-message)";
        case ob::itch::ParseStatus::BadLength: return "BAD LENGTH (corrupt data or framing bug)";
    }
    return "?";
}

unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }

// 1900500 -> "190.0500"
std::string format_price(ob::Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
    return buf;
}

// Prints asks above bids, best prices meeting in the middle, like a trading screen.
void print_book(const ob::ReferenceBook& book, const std::string& symbol, std::size_t levels) {
    const auto locate = book.find_locate(symbol);
    if (!locate) {
        std::printf("\n%s: no Add Order seen for this symbol\n", symbol.c_str());
        return;
    }
    auto asks = book.depth(*locate, ob::Side::Sell, levels);
    auto bids = book.depth(*locate, ob::Side::Buy, levels);
    std::reverse(asks.begin(), asks.end());  // worst ask at top, best ask just above the spread

    std::printf("\n%s (locate %u), top %zu levels\n", symbol.c_str(), *locate, levels);
    std::printf("  %-6s %12s %12s %7s\n", "side", "price", "shares", "orders");
    for (const auto& l : asks)
        std::printf("  %-6s %12s %12llu %7u\n", "ASK", format_price(l.price).c_str(), ull(l.shares), l.orders);
    std::printf("  %s\n", "------------------------------------------");
    for (const auto& l : bids)
        std::printf("  %-6s %12s %12llu %7u\n", "BID", format_price(l.price).c_str(), ull(l.shares), l.orders);
}

}  // namespace

int main(int argc, char** argv) {
    std::string path, book_symbol;
    std::uint64_t max_messages = UINT64_MAX;
    bool have_max = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--book" && i + 1 < argc) {
            book_symbol = argv[++i];
        } else if (path.empty()) {
            path = arg;
        } else if (!have_max && !arg.empty() && std::isdigit(static_cast<unsigned char>(arg[0]))) {
            max_messages = std::stoull(arg);
            have_max = true;
        } else {
            path.clear();
            break;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: %s <itch_file> [max_messages] [--book SYMBOL]\n", argv[0]);
        return 2;
    }

    try {
        const ob::MappedFile file(path);

        CountingHandler handler;
        std::unique_ptr<ob::ReferenceBook> book;
        if (!book_symbol.empty()) {
            book = std::make_unique<ob::ReferenceBook>();
            handler.book = book.get();
        }

        const auto result = ob::itch::parse(file.bytes(), handler, max_messages);

        std::printf("type  count\n");
        for (std::size_t t = 0; t < handler.counts.size(); ++t) {
            if (handler.counts[t] != 0) {
                std::printf("  %c   %llu\n", static_cast<char>(t), ull(handler.counts[t]));
            }
        }
        std::printf("total messages: %llu\n", ull(result.messages));
        std::printf("bytes read:     %zu of %zu\n", result.offset, file.bytes().size());
        std::printf("status:         %s\n", status_name(result.status));

        int rc = result.status == ob::itch::ParseStatus::BadLength ? 1 : 0;

        if (book) {
            const auto& a = book->anomalies();
            std::printf("\nreference book\n");
            std::printf("  live orders:     %zu\n", book->order_count());
            std::printf("  anomalies:       %llu  (unknown ref %llu, duplicate ref %llu, over-reduce %llu, "
                        "zero-share add %llu, bad side %llu)\n",
                        ull(a.total()), ull(a.unknown_ref), ull(a.duplicate_ref), ull(a.over_reduce),
                        ull(a.zero_share_add), ull(a.bad_side));
            const std::string err = book->validate();
            std::printf("  invariant check: %s\n", err.empty() ? "ok" : err.c_str());
            if (!err.empty() || a.total() != 0) rc = 1;

            print_book(*book, book_symbol, 5);
        }
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
