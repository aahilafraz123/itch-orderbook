#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "ob/differential.hpp"
#include "ob/fast_book.hpp"
#include "ob/itch_parser.hpp"
#include "ob/pipeline.hpp"
#include "ob/spsc_ring.hpp"
#include "ob/synthetic.hpp"

using namespace ob;

TEST(SpscRing, CapacityRoundsUpToPowerOfTwo) {
    SpscRing<int> r(100);
    EXPECT_EQ(r.capacity(), 128u);
}

TEST(SpscRing, FullAndEmptySingleThreaded) {
    SpscRing<int> r(4);
    int v = 0;
    EXPECT_FALSE(r.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(99));  // full
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);  // FIFO
    }
    EXPECT_FALSE(r.try_pop(v));
    // Wrap around the end of the buffer many times.
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(r.try_push(i));
        ASSERT_TRUE(r.try_pop(v));
        ASSERT_EQ(v, i);
    }
}

TEST(SpscRing, TwoThreadsDeliverEverythingInOrder) {
    // A tiny ring forces constant full/empty transitions, which is where
    // memory-ordering bugs show up (run under TSan/ASan for extra confidence).
    constexpr std::uint64_t kCount = 5'000'000;
    SpscRing<std::uint64_t> r(64);
    std::uint64_t received = 0, out_of_order = 0, sum = 0;

    std::thread consumer([&] {
        std::uint64_t expect = 0, v = 0;
        while (expect < kCount) {
            if (!r.try_pop(v)) continue;
            if (v != expect) ++out_of_order;
            sum += v;
            ++expect;
        }
        received = expect;
    });
    for (std::uint64_t i = 0; i < kCount; ++i) {
        while (!r.try_push(i)) {}
    }
    consumer.join();

    EXPECT_EQ(received, kCount);
    EXPECT_EQ(out_of_order, 0u);
    EXPECT_EQ(sum, kCount * (kCount - 1) / 2);
}

TEST(Pipeline, ParserAndBookThreadsBuildTheSameBook) {
    const auto bytes = generate_synthetic({.seed = 9, .messages = 300'000});

    FastBook direct;
    itch::parse(bytes, direct);

    SpscRing<Event> ring(256);
    FastBook piped;
    std::thread book_thread([&] {
        Event e;
        for (;;) {
            if (!ring.try_pop(e)) continue;
            if (!apply_event(e, piped)) break;
        }
    });
    RingProducer producer(ring);
    itch::parse(bytes, producer);
    producer.finish();
    book_thread.join();

    EXPECT_EQ(compare_books(direct, piped), "");
    EXPECT_EQ(piped.validate(), "");
}
