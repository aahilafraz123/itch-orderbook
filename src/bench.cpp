// bench: throughput and per-message latency of each order book on a real ITCH file.
//
// Usage: bench <itch_file> [max_messages] [--book NAME] [--runs N] [--expected-orders N] [--no-latency]
//   --book   ref | std-index | binary-search | hybrid | fast | all (default: all)
//   --runs   throughput runs per book; the median is reported (default 5)
//   --expected-orders N  optimized books preallocate for N live orders (default 4M)
//   --levels-reserve N   optimized books reserve N levels per stock-side on first use (default 64)
//   --mmap               replay straight from the memory-mapped file instead of a copy in RAM
//
// Methodology
//   - The file is copied into RAM before timing, so we measure processing,
//     not disk I/O or file paging.
//   - Throughput: parse + book update for the whole replay, timed as one
//     block; messages/sec = all messages / wall time. N runs, fresh book each
//     run (construction not timed), median reported.
//   - Latency: separate replays where each book-changing message (A F E C X D U)
//     is timed individually with std::chrono::steady_clock around the parse +
//     book update of that one message. Samples go into a preallocated
//     histogram (no allocation while measuring). N runs; each percentile is
//     the median of that percentile across runs.
//   - Timer overhead: the same two clock reads around an empty block, reported
//     so latency numbers can be read against it. Not subtracted.
//   - Allocations: global operator new is replaced in this binary to count
//     every heap allocation made during the timed throughput replay.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <sys/mman.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include "ob/fast_book.hpp"
#include "ob/itch_parser.hpp"
#include "ob/latency_recorder.hpp"
#include "ob/mapped_file.hpp"
#include "ob/reference_book.hpp"

// ---- Allocation counting ------------------------------------------------------

namespace {

// macOS can't pin threads to cores. Marking the thread as latency-critical
// makes the scheduler strongly prefer performance cores over efficiency cores.
inline void prefer_performance_cores() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

}  // namespace

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

// GCC flags free() inside a replaced operator delete as "mismatched"; it isn't,
// since our operator new uses malloc.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using Clock = std::chrono::steady_clock;
using ull   = unsigned long long;

std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Prevents the compiler from optimizing away work whose result is unused.
template <class T>
void do_not_optimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

struct Sizing {
    std::size_t expected_orders;  // pool + index preallocation
    std::size_t levels_reserve;   // per stock-side level vector, reserved on first use
};

template <class Book>
std::unique_ptr<Book> make_book(const Sizing& sz) {
    if constexpr (std::is_same_v<Book, ob::ReferenceBook>) {
        (void)sz;
        return std::make_unique<Book>();
    } else {
        return std::make_unique<Book>(
            typename Book::Config{.expected_orders = sz.expected_orders, .levels_reserve = sz.levels_reserve});
    }
}

// Counting pass (untimed): message count plus the data's shape, which drives
// the order-lookup design (is a flat array indexed by order ref feasible?).
struct NullHandler {
    std::uint64_t adds = 0, max_ref = 0;
    void note(std::uint64_t ref) { max_ref = std::max(max_ref, ref); }
    void on_add_order(const ob::itch::AddOrder& m) { ++adds; note(m.order_ref); }
    void on_order_executed(const ob::itch::OrderExecuted&) {}
    void on_order_executed_with_price(const ob::itch::OrderExecutedWithPrice&) {}
    void on_order_cancel(const ob::itch::OrderCancel&) {}
    void on_order_delete(const ob::itch::OrderDelete&) {}
    void on_order_replace(const ob::itch::OrderReplace& m) { ++adds; note(m.new_order_ref); }
    void on_other(char, const std::uint8_t*, std::size_t) {}
};

bool is_book_message(char t) {
    switch (t) {
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U': return true;
        default: return false;
    }
}

struct ThroughputResult {
    double        median_mps = 0, min_mps = 0, max_mps = 0;
    std::uint64_t messages = 0;
    std::uint64_t allocations = 0;         // during the median run
    std::uint64_t allocations_warmup = 0;  // ... of which in the first 10% of messages
    std::size_t   live_orders = 0;
};

template <class Book>
ThroughputResult throughput(std::span<const std::uint8_t> bytes, std::uint64_t total_messages, int runs,
                            const Sizing& sz) {
    struct Run { double mps; std::uint64_t allocs, allocs_warmup; };
    std::vector<Run> results;
    ThroughputResult out;
    const std::uint64_t warmup = total_messages / 10;
    for (int r = 0; r < runs; ++r) {
        auto book = make_book<Book>(sz);
        // Two parse calls (first 10%, then the rest) so allocations can be
        // attributed to warm-up vs steady state. Both are inside the timed block.
        const std::uint64_t a0 = g_allocations.load(std::memory_order_relaxed);
        const auto t0 = Clock::now();
        const auto r1 = ob::itch::parse(bytes, *book, warmup);
        const std::uint64_t a_mid = g_allocations.load(std::memory_order_relaxed);
        const auto r2 = ob::itch::parse(bytes.subspan(r1.offset), *book, total_messages - r1.messages);
        const auto t1 = Clock::now();
        const std::uint64_t a1 = g_allocations.load(std::memory_order_relaxed);
        do_not_optimize(book->order_count());

        const std::uint64_t messages = r1.messages + r2.messages;
        const double secs = static_cast<double>(ns_between(t0, t1)) / 1e9;
        results.push_back({static_cast<double>(messages) / secs, a1 - a0, a_mid - a0});
        out.messages = messages;
        out.live_orders = book->order_count();
    }
    std::sort(results.begin(), results.end(), [](const Run& a, const Run& b) { return a.mps < b.mps; });
    out.min_mps = results.front().mps;
    out.max_mps = results.back().mps;
    out.median_mps = results[results.size() / 2].mps;
    out.allocations = results[results.size() / 2].allocs;
    out.allocations_warmup = results[results.size() / 2].allocs_warmup;
    return out;
}

template <class Book>
ob::LatencyRecorder latency(std::span<const std::uint8_t> bytes, std::uint64_t max_messages, const Sizing& sz) {
    ob::LatencyRecorder rec;  // allocated before timing starts
    auto book = make_book<Book>(sz);
    const std::uint8_t* p = bytes.data();
    const std::size_t size = bytes.size();
    std::size_t pos = 0;
    for (std::uint64_t n = 0; n < max_messages && size - pos >= 2; ++n) {
        const std::size_t len = ob::itch::read_u16(p + pos);
        if (len == 0 || size - pos - 2 < len) break;
        const std::uint8_t* msg = p + pos + 2;
        if (is_book_message(static_cast<char>(msg[0]))) {
            const auto t0 = Clock::now();
            ob::itch::dispatch(msg, len, *book);
            const auto t1 = Clock::now();
            rec.record(ns_between(t0, t1));
        } else {
            ob::itch::dispatch(msg, len, *book);
        }
        pos += 2 + len;
    }
    do_not_optimize(book->order_count());
    return rec;
}

ob::LatencyRecorder timer_overhead() {
    ob::LatencyRecorder rec;
    for (int i = 0; i < 10'000'000; ++i) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        rec.record(ns_between(t0, t1));
    }
    return rec;
}

struct LatencySummary {
    std::uint64_t samples = 0, p50 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    double        mean = 0;
};

LatencySummary summarize(const ob::LatencyRecorder& r) {
    return {r.count(), r.percentile(50), r.percentile(99), r.percentile(99.9), r.percentile(99.99), r.max(), r.mean()};
}

// Element-wise median across runs: each percentile is reported as the median of
// that percentile over all runs, so one noisy run can't set the headline number.
LatencySummary median_of(std::vector<LatencySummary> runs) {
    auto med = [&](auto field) {
        std::vector<std::remove_cvref_t<decltype(runs[0].*field)>> v;
        for (const auto& r : runs) v.push_back(r.*field);
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    return {runs[0].samples, med(&LatencySummary::p50), med(&LatencySummary::p99), med(&LatencySummary::p999),
            med(&LatencySummary::p9999), med(&LatencySummary::max), med(&LatencySummary::mean)};
}

void print_latency_row(const char* name, const LatencySummary& r) {
    std::printf("  %-16s %10llu %8llu %8llu %8llu %8llu %10llu %8.1f\n", name, ull(r.samples), ull(r.p50), ull(r.p99),
                ull(r.p999), ull(r.p9999), ull(r.max), r.mean);
}

template <class Book>
void bench_one(const char* name, std::span<const std::uint8_t> bytes, std::uint64_t total_messages, int runs,
               const Sizing& sz, bool do_latency) {
    const auto t = throughput<Book>(bytes, total_messages, runs, sz);
    std::printf("\n[%s]\n", name);
    std::printf("  throughput: median %.2f M msgs/s (min %.2f, max %.2f over %d runs), %llu messages\n",
                t.median_mps / 1e6, t.min_mps / 1e6, t.max_mps / 1e6, runs, ull(t.messages));
    std::printf("  heap allocations during replay: %llu total; %llu in the first 10%% of messages, %llu after\n",
                ull(t.allocations), ull(t.allocations_warmup), ull(t.allocations - t.allocations_warmup));
    std::printf("  live orders at end: %zu\n", t.live_orders);
    if (do_latency) {
        std::vector<LatencySummary> lruns;
        for (int r = 0; r < runs; ++r) lruns.push_back(summarize(latency<Book>(bytes, total_messages, sz)));
        const LatencySummary l = median_of(lruns);
        std::printf("  latency per book message (ns), median of %d runs per percentile:\n", runs);
        std::printf("  %-16s %10s %8s %8s %8s %8s %10s %8s\n", "", "samples", "p50", "p99", "p99.9", "p99.99", "max", "mean");
        print_latency_row(name, l);
    }
    std::printf("  CSV,%s,%.0f,%.0f,%.0f,%llu,%llu,%llu\n", name, t.median_mps, t.min_mps, t.max_mps, ull(t.messages),
                ull(t.allocations), ull(t.allocations - t.allocations_warmup));
}

}  // namespace

int main(int argc, char** argv) {
    prefer_performance_cores();
    std::string path, which = "all";
    std::uint64_t max_messages = UINT64_MAX;
    int runs = 5;
    Sizing sz{std::size_t{1} << 22, 64};
    bool do_latency = true;
    bool use_mmap = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--book" && i + 1 < argc) which = argv[++i];
            else if (arg == "--runs" && i + 1 < argc) runs = std::stoi(argv[++i]);
            else if (arg == "--expected-orders" && i + 1 < argc) sz.expected_orders = std::stoull(argv[++i]);
            else if (arg == "--levels-reserve" && i + 1 < argc) sz.levels_reserve = std::stoull(argv[++i]);
            else if (arg == "--no-latency") do_latency = false;
            else if (arg == "--mmap") use_mmap = true;
            else if (path.empty()) path = arg;
            else max_messages = std::stoull(arg);
        }
    } catch (const std::exception&) {
        path.clear();  // unknown flag or non-numeric value: print usage
    }
    if (path.empty() || runs < 1) {
        std::fprintf(stderr, "usage: %s <itch_file> [max_messages] [--book ref|std-index|binary-search|hybrid|fast|all] "
                             "[--runs N] [--expected-orders N] [--levels-reserve N] [--no-latency] [--mmap]\n", argv[0]);
        return 2;
    }

    try {
        // Copy the file into ordinary memory before timing, then drop the mapping.
        // Benchmarking straight off the mmap lets the OS page parts of the file
        // back in from disk mid-run, which shows up as ~30 us spikes that have
        // nothing to do with the book. The copy is mlock'ed so the OS can't swap
        // it out under memory pressure either. --mmap skips the copy.
        std::optional<ob::MappedFile> file(std::in_place, path);
        std::vector<std::uint8_t> in_ram;
        std::span<const std::uint8_t> bytes = file->bytes();
        const char* input_mode = "replayed from mmap";
        if (!use_mmap) {
            in_ram.assign(bytes.begin(), bytes.end());
            file.reset();  // unmap: don't hold 2 copies of an 8 GB file
            bytes = in_ram;
            input_mode = ::mlock(in_ram.data(), in_ram.size()) == 0
                             ? "copied into RAM, mlock'ed"
                             : "copied into RAM (mlock FAILED: may be swapped under memory pressure)";
        } else {
            std::uint64_t sum = 0;  // at least touch every page once
            for (std::size_t i = 0; i < bytes.size(); i += 4096) sum += bytes[i];
            do_not_optimize(sum);
        }

        NullHandler null;
        const std::uint64_t total = ob::itch::parse(bytes, null, max_messages).messages;
        std::printf("file: %s (%zu bytes), %llu messages\n", path.c_str(), bytes.size(), ull(total));
        std::printf("orders created (A/F + U): %llu, largest order ref: %llu\n", ull(null.adds), ull(null.max_ref));
        std::printf("sizing: expected_orders %zu, levels_reserve %zu; input %s\n", sz.expected_orders, sz.levels_reserve,
                    input_mode);

        const auto ov = timer_overhead();
        std::printf("timer: std::chrono::steady_clock, smallest nonzero step %llu ns\n", ull(ov.min_nonzero()));
        std::printf("timer overhead (two back-to-back reads, ns): p50 %llu, p99 %llu, p99.9 %llu, p99.99 %llu, max %llu\n",
                    ull(ov.percentile(50)), ull(ov.percentile(99)), ull(ov.percentile(99.9)), ull(ov.percentile(99.99)),
                    ull(ov.max()));

        using namespace ob;
        if (which == "ref" || which == "all")
            bench_one<ReferenceBook>("ref", bytes, total, runs, sz, do_latency);
        if (which == "std-index" || which == "all")
            bench_one<BasicFastBook<StdIndex, LevelSearch::Linear>>("std-index", bytes, total, runs, sz, do_latency);
        if (which == "binary-search" || which == "all")
            bench_one<BasicFastBook<OpenAddressingIndex, LevelSearch::Binary>>("binary-search", bytes, total, runs, sz, do_latency);
        if (which == "hybrid" || which == "all")
            bench_one<BasicFastBook<OpenAddressingIndex, LevelSearch::Hybrid>>("hybrid", bytes, total, runs, sz, do_latency);
        if (which == "fast" || which == "all")
            bench_one<FastBook>("fast", bytes, total, runs, sz, do_latency);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
