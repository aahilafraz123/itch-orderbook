# itch-orderbook

A C++20 limit order book that replays a full day of real **NASDAQ TotalView-ITCH 5.0** market data,
rebuilds every stock's order book message by message, proves the optimized book is correct
against a simple reference implementation, and benchmarks throughput and per-message latency.

Trading systems maintain their own copy of the exchange's order book, built from feeds like ITCH.
That copy has to be exactly right and update within microseconds: a stale book means quoting and
trading on prices that no longer exist.

**Results** (NASDAQ 2019-12-30, one Apple M5 Pro core):

- **Correct on a full trading day.** The optimized book matches the reference book after every
  one of 268.7M messages, down to the FIFO queue at every price level.
- **12.7M messages/s, 4.1× the reference book.** Per book update: p50 83 ns, p99 417 ns,
  p99.9 792 ns (median of 5 runs over 69.4M messages).
- **Almost no allocation once trading starts:** 3,598 heap allocations over 62M messages,
  against 80M for the reference book.

---

## Contents

- [What it does](#what-it-does)
- [Architecture](#architecture)
- [Repository layout](#repository-layout)
- [Design decisions](#design-decisions)
- [Correctness](#correctness)
- [Benchmarks](#benchmarks)
- [Stretch: SPSC pipeline and matching engine](#stretch-spsc-pipeline-and-matching-engine)
- [Limitations and future work](#limitations-and-future-work)
- [How to run](#how-to-run)

## What it does

1. **Parses** the binary ITCH 5.0 stream: 2-byte length-framed messages, big-endian integers,
   prices as integers with 4 implied decimals. Decodes the seven message types that change the
   book (`A` `F` add, `E` `C` execute, `X` partial cancel, `D` delete, `U` replace); every other
   type is skipped safely by length. Offsets were checked against the official spec (Feb 2024 edition).
2. **Rebuilds the full limit order book** for every stock: bids and asks at every price level,
   and the FIFO queue of orders at each level (time priority).
3. **Proves correctness** with a differential test: the optimized book and a slow, obviously correct
   reference book replay the same messages and are compared continuously, on a whole real trading
   day and on synthetic edge-case streams in CI.
4. **Benchmarks** throughput (messages/sec) and per-message latency (p50/p99/p99.9/p99.99/max),
   counts heap allocations during the replay, and compares each optimization against the baseline.
5. Ships with CMake, 165 GoogleTest tests, sanitizer builds, Docker and GitHub Actions CI.

## Architecture

```
                 mmap'd ITCH file (8.25 GB, 268.7M messages)
                               │
                               ▼
        ┌──────────────────────────────────────────────┐
        │ itch::parse()        framing + length checks │   itch_parser.hpp
        │ itch::dispatch()     switch on type byte      │
        │ decode_*()           memcpy + byteswap        │   itch_messages.hpp
        └──────────────────────────────────────────────┘
                               │  compile-time Handler (template, no virtual calls)
            ┌──────────────────┼───────────────────────────────┐
            ▼                  ▼                               ▼
     ReferenceBook          FastBook                   DifferentialHandler
     std::unordered_map     order pool + free list     feeds both books, compares
     std::map levels        intrusive FIFO lists       after every message
     std::list queues       open-addressing index
                            sorted vector levels,
                            best price at back

   FastBook, per stock (locate 0..65535) and side:

     levels (sorted by key, best at back)          order pool (32-byte Orders)
     ┌────────┬────────┬────────┬────────┐        ┌─────┬─────┬─────┬─────┬───
     │ $189.97│ $189.98│ $189.99│ $190.00│ ◄best  │ #17 │ #42 │ free│ #9  │ ...
     │ head ──┼────────┼────────┼─► #42 ─┼──────► │     │next─┼─────┼► #9 │
     └────────┴────────┴────────┴────────┘        └─────┴─────┴─────┴─────┴───
                                                       ▲
     order index: open-addressing table  ref ──► pool slot
```

## Repository layout

```
include/ob/
  itch_messages.hpp    big-endian readers, decoded message structs, decoders
  itch_parser.hpp      framing loop + compile-time dispatch to a Handler
  book_types.hpp       Price/Shares/Locate types, LevelSummary, BookAnomalies (shared by all books)
  reference_book.hpp   slow, obviously correct book (ground truth)
  fast_book.hpp        optimized book (policy-templated so each optimization can be measured)
  order_index.hpp      StdIndex (std::unordered_map) and OpenAddressingIndex
  differential.hpp     book-vs-book comparison + DifferentialHandler
  latency_recorder.hpp preallocated nanosecond histogram
  mapped_file.hpp      read-only mmap
  itch_writer.hpp      ITCH encoder (synthetic streams only)
  synthetic.hpp        seeded edge-case stream generator for CI
  spsc_ring.hpp        lock-free single-producer/single-consumer ring
  pipeline.hpp         parser-thread → ring → book-thread glue
  matching_engine.hpp  price-time priority matching engine
src/
  main.cpp             itch_stats: message counts, --book SYMBOL prints a stock's book
  differential.cpp     differential: fast vs reference over a whole file
  bench.cpp            bench: throughput, latency, allocations, every variant
  pipeline.cpp         pipeline: single thread vs 2-thread SPSC pipeline
tests/                 parser, books (typed: every book runs every test), differential,
                       matching engine, SPSC ring + pipeline
scripts/
  fetch_sample.sh      download / prefix / decompress NASDAQ sample files
  bench_linux.sh       full benchmark + perf run for real Linux hardware
results/               raw output of every run quoted in this README
```

## Design decisions

### Parser
- **`memcpy` + byte swap, never pointer casts.** ITCH integers are big-endian and usually
  misaligned; `*(uint32_t*)p` is undefined behavior (alignment and strict aliasing). A fixed-size
  `memcpy` compiles to one load, so it's free. Tests include unaligned reads and run under UBSan.
- **Compile-time dispatch.** The handler is a template parameter constrained by a C++20 concept,
  so `on_add_order()` and friends inline into the parser's `switch`. A virtual interface would
  add an indirect call per message that the optimizer can't see through.
- **Exact length check per type.** A known type with the wrong length stops the parse
  (`BadLength`) rather than decoding garbage. A cut-off final message is reported as `Truncated`,
  which is expected when working on a prefix of a file.

### Reference book
`std::unordered_map` of orders, a `std::map` of levels per stock and side (bids high-to-low, asks
low-to-high, so `begin()` is always best), and a `std::list` FIFO per level. Each order holds an
iterator to its own queue slot for O(1) removal. It tracks time priority, not just totals, so the
differential test catches priority bugs too. `validate()` checks every internal invariant.

### Anomaly policy (both books, identical)
Unknown order refs, duplicate refs, over-executions, zero-share adds and bad side bytes are
**counted and otherwise ignored** (an over-execution removes the order, since it can't have shares
left). On a full day replayed from the start, every counter must be 0, and it is.

### Fast book
| Concern | Choice | Why |
|---|---|---|
| Order storage | preallocated `std::vector<Order>` pool + free list; 32-byte orders | no allocation per order; two orders per cache line; slots reused |
| FIFO queues | intrusive doubly linked list (prev/next **indices** in each Order) | O(1) unlink, no list nodes; indices survive pool growth |
| ref → order | open-addressing hash table: linear probing, Fibonacci hashing, backward-shift deletion, ≤50% load | one flat array, usually one cache line per lookup; no tombstones, so probes don't degrade over hundreds of millions of deletes |
| Price levels | per stock-side sorted `std::vector<Level>`, **best price at the back**; asks keyed by `~price` so both sides share one ascending implementation | most activity is at or near the best price, so inserts/erases there move few elements; best bid/ask is `back()` |
| Level lookup | linear scan from the best price outward | measured faster than binary search and than a hybrid (see benchmarks) |
| Allocation | pool and index sized up front; each stock's level storage reserved when its Stock Directory (`R`) message arrives before trading | the trading day itself allocates almost nothing (measured below) |

**Why a hash table and not a flat array for ref → order?** On the 2019-12-30 file, 140,270,523
orders are created (adds plus replaces) and the largest order ref is 308,631,072. Refs are close to
sequential, so a direct-indexed array is possible, but it would need 308.6M entries (1.2 GB of
4-byte slot indices) to serve a book that holds a few million live orders at a time. The
open-addressing table is 128 MB at 4M expected orders, and stays at most half full.

## Correctness

**Full trading day, fast vs reference** (`results/differential_full_day.txt`, NASDAQ 2019-12-30):

| | |
|---|---|
| messages replayed | 268,744,780 |
| top-of-book comparisons (after every book-changing message) | 263,241,937 |
| full-depth comparisons of every touched stock (every 1,000 messages) | 263,237 |
| final comparison | every level and every FIFO queue of every stock, live-order counts, anomaly counters |
| anomalies in the reference book | 0 |
| live orders at end of day | 0 (every order is filled or canceled by the close) |
| result | **MATCH** |

**Tests** (`ctest`, 165 tests, all run in CI on GCC and Clang, plus ASan/UBSan):

| Suite | What it covers |
|---|---|
| `test_parser` (26) | every field of every decoded type from **hand-written byte literals** (not an encoder, so encoder and decoder can't share a misunderstanding); big-endian readers incl. unaligned and high-bit cases; framing: unknown types skipped, truncation, wrong/zero lengths, message limits |
| `test_books` (22 × 5 books) | **typed tests**: every case runs against the reference book and all four optimized variants, and every test ends with a full `validate()` of the book's internal invariants. Partial/full executes, execute-with-price, partial/full cancels, deletes from mid-queue, emptying the best level, replaces (new price, same price → loses priority, addressed by new ref), every anomaly type, end-to-end through the parser |
| `test_differential` (3 × 4 + 2) | each optimized variant vs reference on seeded synthetic streams, **compared after every message**; deep books (200+ levels); pool and index forced to grow mid-stream; a deliberately broken book (drops every 500th cancel) must be caught and reported with its message number; optional real-data run via `ITCH_SAMPLE` |
| `test_matching_engine` (11) | price then time priority, maker-price execution, partial fills keep priority, level sweeps, market orders, cancels, rejects; randomized 20,000-order test: book never crosses, shares conserved |
| `test_spsc_ring` (4) | full/empty/wraparound, 5M ordered elements across two threads on a 64-slot ring, pipeline builds the identical book |

**Mutation checks**: tests are only evidence if they fail when the code is wrong. A decode offset
(Replace's price), FIFO insertion (new orders to the front of the queue) and a cancel path (the
broken-book test above) were each deliberately broken; in every case the expected tests failed.

**Sanitizers**: the whole suite passes under AddressSanitizer + UBSan; the ring and pipeline tests
pass under ThreadSanitizer.

## Benchmarks

**Setup.** NASDAQ TotalView-ITCH 5.0, 2019-12-30, the first 2 GiB of the day's file:
69,406,748 messages, 67,804,031 of them book updates. Apple M5 Pro, 24 GB, macOS 26.6,
Apple clang 21, `-O3 -march=native`, on AC power with the machine otherwise idle. Raw output:
[`results/bench_macos.txt`](results/bench_macos.txt).

**Method** (`src/bench.cpp`):
- The input is copied into RAM and `mlock`ed before timing, and the file mapping is dropped, so
  results measure processing, not disk I/O, file paging or swap. Two earlier methodology bugs drove
  this. Replaying straight from an `mmap` whose pages weren't all resident produced ~30 µs p99.99
  spikes from page faults. An unlocked 8 GB copy was partly swapped out under memory pressure.
  Neither had anything to do with the book.
- **Why a 2 GiB prefix:** locking the whole 8.25 GB day in RAM, alongside the books, pushes a
  24 GB laptop into swap, which would measure the swap device, not the book. Correctness is
  checked on the whole day (above).
- **Throughput**: parse + book update for the whole input, timed as one block. 5 runs, a fresh
  book each run (construction untimed), median reported along with the min–max range.
- **Latency**: each book-changing message (`A F E C X D U`) timed individually with
  `std::chrono::steady_clock` around its decode + book update, into a preallocated nanosecond
  histogram. 5 runs; each percentile is the median of that percentile across runs.
- **Timer**: Apple Silicon's counter ticks every 41.7 ns, so latencies are quantized to that step.
  Two back-to-back clock reads measure p50 0 ns, p99 42 ns; numbers below include that overhead
  (not subtracted).
- **Allocations**: global `operator new` is replaced in the benchmark binary and counted during
  the timed replay, split into the first 10% of messages (start-of-day warm-up) and the rest.
- macOS gets a `QOS_CLASS_USER_INTERACTIVE` hint so the scheduler keeps the run on a performance
  core. Without it, runs that landed on an efficiency core came out many times slower.

### Each optimization, measured

Every row is a real configuration of the code (`BasicFastBook<Index, LevelSearch>`), so each change
is measured in isolation. All variants pass the same 22 book tests and the differential tests.

| Configuration | Throughput (M msgs/s) | Range, 5 runs | vs. previous | Heap allocs after warm-up |
|---|---:|---:|---:|---:|
| **Reference book**: `unordered_map` orders, `std::map` levels, `std::list` queues | 3.08 | 2.71–3.28 | | 80,056,270 |
| **+ order pool, intrusive FIFO lists, sorted-vector levels** (index still `std::unordered_map`) | 6.27 | 5.35–6.63 | **2.0×** | 34,076,748 |
| **+ open-addressing order index** (binary-search level lookup) | 11.27 | 11.15–11.39 | **1.8×** | 3,598 |
| **+ linear level lookup from the best price** (shipped `FastBook`) | **12.70** | 12.44–12.84 | **1.13×** | 3,598 |
| Hybrid lookup: linear over the 8 best levels, then binary (experiment) | 12.17 | 11.67–12.61 | 0.96× vs. `FastBook` | 3,598 |

Overall: **4.1× the reference book's throughput**. Heap allocations after warm-up drop from more
than one per message (80.1M) to **3,598**. Those come from stocks whose books grow deeper than the
64 price levels reserved for each stock-side when its Stock Directory message arrives. Rerunning
with `--levels-reserve 1024` brings the count to 25
([`results/bench_levels_reserve_1024.txt`](results/bench_levels_reserve_1024.txt)), at the cost of reserving more memory for
thousands of thinly traded stocks. The ~18,000 allocations in the warm-up window are those
reservations themselves, made before the market opens.

The hybrid lookup was meant to cap the cost of long scans in deep books. It's slightly slower
than the plain linear scan here, although the run ranges overlap, so `FastBook` keeps the
simpler linear scan.

### Latency per message (ns)

| Book | p50 | p99 | p99.9 | p99.99 | mean |
|---|---:|---:|---:|---:|---:|
| Reference | 208 | 1,083 | 1,542 | 11,750 | 267 |
| std::unordered_map index | 83 | 750 | 1,083 | 5,959 | 174 |
| Binary-search levels | 83 | 542 | 917 | 4,667 | 103 |
| Hybrid levels | 83 | 500 | 916 | 4,583 | 98 |
| **FastBook** | **83** | **417** | **792** | **4,417** | **95** |

The p50 of every optimized variant is two ticks of the 41.7 ns counter, so the timer can't separate
them there; the mean and the tail can, and they order the variants the same way as throughput.
p99.99 and the maximum (in the raw output) include OS noise: macOS can't pin a thread to a core or
keep interrupts off it, so the extreme tail mixes in scheduler and interrupt time.

### Profile

`perf record -e cpu-clock` of the fast book in the Linux container, on the same 2 GiB input
([`results/perf_fast_report.txt`](results/perf_fast_report.txt)). `remove()` takes 36% of all
samples. It's the path for every delete, full execution and replace. `add()` takes 16% and the
parser's dispatch 9%. Another 16% is kernel time. The profile covers the whole process, so that
includes loading the input and prefaulting memory before the timed replay.

Annotating `remove()` ([`results/perf_remove_annotate.txt`](results/perf_remove_annotate.txt))
shows memory stalls, not computation:

- **~40%: first read of the stock's level array.** The best level is usually the one it needs, so
  this is a cache miss, not a long search.
- **~31%: loading the order's 32-byte record from the pool.**
- **~17%: the linear level scan itself.**

The open-addressing index lookup barely registers. The cost is two dependent misses at random
addresses (order record, then level array) in a working set of a few hundred MB. That's why the
hybrid lookup didn't help: it shortens the scan, which isn't where the time goes.

## Stretch: SPSC pipeline and matching engine

### Two-thread pipeline (`spsc_ring.hpp`, `pipeline.hpp`, `src/pipeline.cpp`)

A parser thread decodes messages into 72-byte events and pushes them into a lock-free
single-producer/single-consumer ring; a book thread pops and applies them.

- One writer per index: the producer owns `tail`, the consumer owns `head`. No locks, no CAS.
- The producer writes the slot, then publishes `tail` with a **release** store; the consumer reads
  `tail` with an **acquire** load, so seeing the new tail guarantees seeing the slot's contents.
  The same pairing runs in reverse on `head`.
- `head` and `tail` sit in separate 128-byte-aligned blocks (Apple Silicon cache lines are 128
  bytes) so the two cores don't false-share a line. Each side caches the other's index and only
  re-reads it when the ring looks full or empty.
- Verified: 5M ordered values across threads through a 64-slot ring, ThreadSanitizer clean, and
  the pipelined book is compared against a single-threaded one after every run.

Results on the same 2 GiB input ([`results/pipeline_macos.txt`](results/pipeline_macos.txt)):

| | Throughput | Latency p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Single thread | 10.84 M msgs/s | 83 ns | 500 ns | 875 ns |
| Parser thread → ring → book thread | 9.03 M msgs/s | 250 ns | 708 ns | 1,584 ns |

Latency here is "message available → applied to the book" with one message in flight. Throughput
is a single run of each, so the single-thread figure sits below the 5-run median in the table
above. The pipelined book is checked against the single-threaded one after the run, and they're
equal.

The pipeline is **slower** for this workload. A book update costs tens of nanoseconds, less than
moving each event's cache lines from one core to another (≈170 ns added at p50). Splitting work
across threads only pays when the per-message work is larger than the handoff, for example
strategy logic on the book thread.

### Matching engine (`matching_engine.hpp`)

The exchange's side: accepts limit and market orders and cancels, and generates fills. Price
priority, then time priority; trades execute at the resting order's price; partially filled resting
orders keep their place; market orders never rest. Covered by 11 tests, including a randomized
20,000-order test asserting the book never crosses and shares are conserved.

## Limitations and future work

- **Measurement platform.** The published numbers come from an Apple M5 Pro (macOS, plus an arm64
  Linux VM under Docker). Apple Silicon's system counter ticks at 24 MHz, so per-message latencies
  are quantized to ~41.7 ns steps, and the hypervisor exposes no hardware performance counters to
  the Linux VM (`perf stat` reports cycles and cache misses as `<not supported>`; profiling used
  `perf record -e cpu-clock` sampling). `scripts/bench_linux.sh` reruns everything on bare-metal
  Linux, pinned with `taskset`, with full `perf stat` counters.
- **Remaining hot spot.** Profiling shows the cost is now dominated by two dependent cache misses
  per removal (the order record, then that stock's level array), at random addresses in a large
  working set. Next steps would be:
  - keeping each stock-side's best few levels inline next to its vector header, so the common
    case skips one miss;
  - prefetching the next message's order slot while the current one is applied;
  - huge pages for the pool and index, to cut TLB misses.
- **Order-ref → slot as a flat array.** NASDAQ order refs are assigned nearly sequentially, so a
  direct-indexed array is feasible (see data shape above) at the cost of memory proportional to
  the largest ref rather than the live order count. Worth measuring against the hash table.
- **Pipeline granularity.** The two-thread pipeline hands over one 72-byte event per message;
  batching events or passing raw message pointers would cut the cross-core traffic.
- **Scope.** Single trading day, single feed, no packet-level (MoldUDP64) handling, recovery or gap
  detection; the matching engine handles one instrument with limit, market and cancel.

## How to run

Requirements: CMake ≥ 3.20, a C++20 compiler (GCC 13+ or Clang 16+), Ninja optional.

```bash
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure       # 165 tests incl. synthetic differential
```

Get real data (3.5 GB download, 8.25 GB decompressed):

```bash
scripts/fetch_sample.sh download          # 12302019.NASDAQ_ITCH50.gz, verified with gzip -t
scripts/fetch_sample.sh prefix 200        # first 200 MB, usable while still downloading
scripts/fetch_sample.sh decompress        # whole day
scripts/fetch_sample.sh prefix 2048       # first 2 GiB, the benchmark input used above
```

Tools:

```bash
./build/itch_stats data/12302019.NASDAQ_ITCH50 --book AAPL     # counts + AAPL's book at end of file
./build/differential data/12302019.NASDAQ_ITCH50               # fast vs reference, whole day
./build/bench data/12302019.NASDAQ_ITCH50.prefix2048MB --runs 5   # every variant, throughput + latency
./build/pipeline data/12302019.NASDAQ_ITCH50.prefix2048MB         # 1 thread vs 2-thread SPSC
ITCH_SAMPLE=data/12302019.NASDAQ_ITCH50 ctest --test-dir build -R RealData   # differential as a test
scripts/bench_linux.sh data/12302019.NASDAQ_ITCH50 2           # on Linux: pinned, perf stat/record
```

Sanitizers and Docker:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan
cmake -S . -B build-tsan -DOB_TSAN=ON && cmake --build build-tsan --target test_spsc_ring && ./build-tsan/test_spsc_ring
docker build -t itch-orderbook .                                # builds and runs all tests on Linux/GCC
```
