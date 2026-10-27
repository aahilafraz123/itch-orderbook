// differential: replay an ITCH file into the reference book and an optimized
// book at the same time and prove they agree.
//
// Usage: differential <itch_file> [max_messages] [--every K] [--variant NAME]
//   --every K       full-depth comparison of touched stocks every K messages (default 1000).
//                   Best bid/ask of the touched stock is compared after EVERY message,
//                   and everything (incl. FIFO queues) is compared at the end.
//   --variant NAME  fast (default) | std-index | binary-search | hybrid
//
// Exit code 0 only if the books agree everywhere, both pass validate(),
// the parse had no framing errors and the reference book saw no anomalies.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "ob/differential.hpp"
#include "ob/fast_book.hpp"
#include "ob/itch_parser.hpp"
#include "ob/mapped_file.hpp"
#include "ob/reference_book.hpp"

namespace {

template <class Fast>
int run(const ob::MappedFile& file, std::uint64_t max_messages, std::uint64_t every, const char* name) {
    const auto t0 = std::chrono::steady_clock::now();

    ob::ReferenceBook ref;
    Fast fast(typename Fast::Config{.expected_orders = std::size_t{1} << 23});
    ob::DifferentialHandler<ob::ReferenceBook, Fast> diff(ref, fast, every);

    const auto r = ob::itch::parse(file.bytes(), diff, max_messages);
    std::string err = r.status == ob::itch::ParseStatus::BadLength
                          ? "framing error at byte " + std::to_string(r.offset)
                          : diff.finish();
    if (err.empty()) {
        if (auto e = ref.validate(); !e.empty()) err = "reference book invalid: " + e;
        else if (auto e2 = fast.validate(); !e2.empty()) err = "fast book invalid: " + e2;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const auto& a = ref.anomalies();
    std::printf("variant:            %s\n", name);
    std::printf("messages replayed:  %llu\n", static_cast<unsigned long long>(diff.messages()));
    std::printf("top-of-book checks: %llu (after every book-changing message)\n", static_cast<unsigned long long>(diff.book_messages()));
    std::printf("full-depth checks:  %llu (every %llu messages) + final full check incl. queues\n",
                static_cast<unsigned long long>(diff.full_checks()), static_cast<unsigned long long>(every));
    std::printf("live orders at end: %zu\n", ref.order_count());
    std::printf("anomalies (ref):    %llu\n", static_cast<unsigned long long>(a.total()));
    std::printf("wall time:          %.1f s\n", secs);
    std::printf("result:             %s\n", err.empty() ? "MATCH" : ("MISMATCH: " + err).c_str());
    return err.empty() && a.total() == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path, variant = "fast";
    std::uint64_t max_messages = UINT64_MAX, every = 1000;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--every" && i + 1 < argc) every = std::stoull(argv[++i]);
            else if (arg == "--variant" && i + 1 < argc) variant = argv[++i];
            else if (path.empty()) path = arg;
            else max_messages = std::stoull(arg);
        }
    } catch (const std::exception&) {
        path.clear();  // unknown flag or non-numeric value: print usage
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: %s <itch_file> [max_messages] [--every K] [--variant fast|std-index|binary-search|hybrid]\n", argv[0]);
        return 2;
    }
    try {
        const ob::MappedFile file(path);
        using namespace ob;
        if (variant == "fast") return run<FastBook>(file, max_messages, every, "fast");
        if (variant == "std-index") return run<BasicFastBook<StdIndex, LevelSearch::Linear>>(file, max_messages, every, "std-index");
        if (variant == "binary-search") return run<BasicFastBook<OpenAddressingIndex, LevelSearch::Binary>>(file, max_messages, every, "binary-search");
        if (variant == "hybrid") return run<BasicFastBook<OpenAddressingIndex, LevelSearch::Hybrid>>(file, max_messages, every, "hybrid");
        std::fprintf(stderr, "unknown variant %s\n", variant.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
