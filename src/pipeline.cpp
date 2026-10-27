// pipeline: single-threaded replay vs a two-thread parser -> SPSC ring -> book pipeline.
//
// Usage: pipeline <itch_file> [max_messages] [--cpus P,C]
//   --cpus P,C  (Linux only) pin the parser thread to CPU P and the book thread to CPU C
//
// Reports, for the same messages and the same FastBook:
//   1. Throughput: single thread vs pipelined, both replaying flat out.
//   2. Latency, unloaded: the parser hands over one message, then waits until the book
//      thread has applied it before handing over the next. Time is measured from "message
//      available" (before decoding) to "applied to the book". Single-threaded, that's
//      decode + apply on one core; pipelined it adds the cross-core handoff through the ring.
//      Book-changing messages only, same steady_clock as bench.
// Correctness: after each pipelined replay the book must equal a single-threaded one.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "ob/differential.hpp"
#include "ob/fast_book.hpp"
#include "ob/itch_parser.hpp"
#include "ob/latency_recorder.hpp"
#include "ob/mapped_file.hpp"
#include "ob/pipeline.hpp"

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

using ull = unsigned long long;

void pin_to(int cpu) {
    prefer_performance_cores();
#ifdef __linux__
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#else
    (void)cpu;
#endif
}

bool is_book_message(char t) {
    return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

ob::FastBook::Config sizing() { return {.expected_orders = std::size_t{1} << 22, .levels_reserve = 64}; }

// Calls f(msg, len) for each framed message, up to max_messages.
template <class F>
std::uint64_t for_each_message(std::span<const std::uint8_t> bytes, std::uint64_t max_messages, F&& f) {
    const std::uint8_t* p = bytes.data();
    std::size_t pos = 0, n = 0;
    while (n < max_messages && bytes.size() - pos >= 2) {
        const std::size_t len = ob::itch::read_u16(p + pos);
        if (len == 0 || bytes.size() - pos - 2 < len) break;
        f(p + pos + 2, len);
        pos += 2 + len;
        ++n;
    }
    return n;
}

void print_latency(const char* name, const ob::LatencyRecorder& r) {
    std::printf("  %-14s %10llu %7llu %7llu %7llu %8llu %10llu %8.1f\n", name, ull(r.count()), ull(r.percentile(50)),
                ull(r.percentile(99)), ull(r.percentile(99.9)), ull(r.percentile(99.99)), ull(r.max()), r.mean());
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::uint64_t max_messages = UINT64_MAX;
    int cpu_parser = -1, cpu_book = -1;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--cpus" && i + 1 < argc) {
                const std::string v = argv[++i];
                const auto comma = v.find(',');
                if (comma == std::string::npos) throw std::invalid_argument("--cpus");
                cpu_parser = std::stoi(v.substr(0, comma));
                cpu_book = std::stoi(v.substr(comma + 1));
            } else if (path.empty()) {
                path = arg;
            } else {
                max_messages = std::stoull(arg);
            }
        }
    } catch (const std::exception&) {
        path.clear();  // unknown flag or non-numeric value: print usage
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: %s <itch_file> [max_messages] [--cpus P,C]\n", argv[0]);
        return 2;
    }

    try {
        // Replay from an mlock'ed copy in RAM, like bench, so file paging and
        // swapping don't show up as latency.
        std::vector<std::uint8_t> in_ram;
        {
            const ob::MappedFile file(path);
            in_ram.assign(file.bytes().begin(), file.bytes().end());
        }
        const bool locked = ::mlock(in_ram.data(), in_ram.size()) == 0;
        const std::span<const std::uint8_t> bytes = in_ram;
        pin_to(cpu_parser);

        // ---- 1a. single-threaded throughput ----
        ob::FastBook single(sizing());
        auto t0 = std::chrono::steady_clock::now();
        const auto res = ob::itch::parse(bytes, single, max_messages);
        auto t1 = std::chrono::steady_clock::now();
        const double single_secs = std::chrono::duration<double>(t1 - t0).count();
        const std::uint64_t n = res.messages;

        // ---- 1b. pipelined throughput ----
        ob::SpscRing<ob::Event> ring(1 << 16);
        ob::FastBook piped(sizing());
        t0 = std::chrono::steady_clock::now();
        std::thread consumer([&] {
            pin_to(cpu_book);
            ob::Event e;
            for (;;) {
                if (!ring.try_pop(e)) continue;
                if (!ob::apply_event(e, piped)) break;
            }
        });
        ob::RingProducer producer(ring);
        ob::itch::parse(bytes, producer, max_messages);
        producer.finish();
        consumer.join();
        t1 = std::chrono::steady_clock::now();
        const double piped_secs = std::chrono::duration<double>(t1 - t0).count();
        std::string err = ob::compare_books(single, piped);

        std::printf("messages: %llu (input copied into RAM%s)\n", ull(n), locked ? ", mlock'ed" : ", mlock FAILED");
        std::printf("ring: %zu slots of %zu bytes\n", ring.capacity(), sizeof(ob::Event));
#ifdef __linux__
        std::printf("pinning: parser cpu %d, book cpu %d\n", cpu_parser, cpu_book);
#else
        std::printf("pinning: not available on this OS (threads float)\n");
#endif
        std::printf("\nthroughput (flat out)\n");
        std::printf("  single thread: %.2f M msgs/s\n", static_cast<double>(n) / single_secs / 1e6);
        std::printf("  pipelined:     %.2f M msgs/s\n", static_cast<double>(n) / piped_secs / 1e6);

        // ---- 2a. unloaded latency, single thread ----
        ob::LatencyRecorder lat_single, lat_piped;
        {
            ob::FastBook book(sizing());
            for_each_message(bytes, max_messages, [&](const std::uint8_t* msg, std::size_t len) {
                const std::int64_t a = ob::now_ns();
                ob::itch::dispatch(msg, len, book);
                if (is_book_message(static_cast<char>(msg[0]))) {
                    lat_single.record(static_cast<std::uint64_t>(ob::now_ns() - a));
                }
            });
        }

        // ---- 2b. unloaded latency, pipelined (one message in flight at a time) ----
        {
            ob::SpscRing<ob::Event> ring2(1 << 16);
            ob::FastBook book(sizing());
            std::atomic<std::uint64_t> applied{0};
            std::thread c([&] {
                pin_to(cpu_book);
                ob::Event e;
                std::uint64_t done = 0;
                for (;;) {
                    if (!ring2.try_pop(e)) continue;
                    if (!ob::apply_event(e, book)) break;
                    if (e.kind != ob::Event::Kind::Other) {
                        lat_piped.record(static_cast<std::uint64_t>(ob::now_ns() - e.stamp_ns));
                    }
                    applied.store(++done, std::memory_order_release);
                }
            });
            ob::RingProducer prod(ring2);
            for_each_message(bytes, max_messages, [&](const std::uint8_t* msg, std::size_t len) {
                prod.stamp_ns = ob::now_ns();
                ob::itch::dispatch(msg, len, prod);
                while (applied.load(std::memory_order_acquire) != prod.pushed()) {}  // wait for the book thread
            });
            prod.finish();
            c.join();
            if (err.empty()) err = ob::compare_books(single, book);
        }

        std::printf("\nlatency, one message in flight (ns, available -> applied to book)\n");
        std::printf("  %-14s %10s %7s %7s %7s %8s %10s %8s\n", "", "samples", "p50", "p99", "p99.9", "p99.99", "max", "mean");
        print_latency("single thread", lat_single);
        print_latency("pipelined", lat_piped);

        std::printf("\npipelined books equal single-threaded book: %s\n", err.empty() ? "yes" : err.c_str());
        return err.empty() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
