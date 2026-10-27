// itch_stats: replay an ITCH 5.0 file and print how many messages of each type it has.
//
// Usage: itch_stats <file> [max_messages]
//   file          decompressed NASDAQ ITCH 5.0 file (e.g. 01302019.NASDAQ_ITCH50)
//   max_messages  stop after this many messages (handy for a quick look at a huge file)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "ob/itch_parser.hpp"
#include "ob/mapped_file.hpp"

namespace {

struct CountingHandler {
    std::array<std::uint64_t, 256> counts{};

    void count(char type) { ++counts[static_cast<unsigned char>(type)]; }

    void on_add_order(const ob::itch::AddOrder& m)                                { count(m.header.type); }
    void on_order_executed(const ob::itch::OrderExecuted& m)                      { count(m.header.type); }
    void on_order_executed_with_price(const ob::itch::OrderExecutedWithPrice& m)  { count(m.header.type); }
    void on_order_cancel(const ob::itch::OrderCancel& m)                          { count(m.header.type); }
    void on_order_delete(const ob::itch::OrderDelete& m)                          { count(m.header.type); }
    void on_order_replace(const ob::itch::OrderReplace& m)                        { count(m.header.type); }
    void on_other(char type, const std::uint8_t*, std::size_t)                    { count(type); }
};

const char* status_name(ob::itch::ParseStatus s) {
    switch (s) {
        case ob::itch::ParseStatus::Ok:        return "ok";
        case ob::itch::ParseStatus::Truncated: return "truncated (file ends mid-message)";
        case ob::itch::ParseStatus::BadLength: return "BAD LENGTH (corrupt data or framing bug)";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <itch_file> [max_messages]\n", argv[0]);
        return 2;
    }

    try {
        const ob::MappedFile file(argv[1]);
        const std::uint64_t max_messages =
            argc == 3 ? std::stoull(argv[2]) : UINT64_MAX;

        CountingHandler handler;
        const auto result = ob::itch::parse(file.bytes(), handler, max_messages);

        std::printf("type  count\n");
        for (std::size_t t = 0; t < handler.counts.size(); ++t) {
            if (handler.counts[t] != 0) {
                std::printf("  %c   %llu\n", static_cast<char>(t),
                            static_cast<unsigned long long>(handler.counts[t]));
            }
        }
        std::printf("total messages: %llu\n", static_cast<unsigned long long>(result.messages));
        std::printf("bytes read:     %zu of %zu\n", result.offset, file.bytes().size());
        std::printf("status:         %s\n", status_name(result.status));

        return result.status == ob::itch::ParseStatus::BadLength ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
