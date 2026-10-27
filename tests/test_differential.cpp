// Differential tests: optimized books vs the reference book, message by message.
//
// - Synthetic streams (always run, including in CI): several seeds, every
//   optimized variant, full depth of the touched stock compared after every
//   message, everything (including FIFO queues) compared at the end.
// - Real NASDAQ data (local only): set ITCH_SAMPLE=/path/to/file and optionally
//   ITCH_SAMPLE_MESSAGES=N (default 5,000,000).

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "ob/differential.hpp"
#include "ob/fast_book.hpp"
#include "ob/itch_parser.hpp"
#include "ob/mapped_file.hpp"
#include "ob/reference_book.hpp"
#include "ob/synthetic.hpp"

using namespace ob;

namespace {

template <class Fast>
class Differential : public ::testing::Test {};

using FastTypes = ::testing::Types<FastBook,
                                   BasicFastBook<StdIndex, LevelSearch::Linear>,
                                   BasicFastBook<OpenAddressingIndex, LevelSearch::Binary>,
                                   BasicFastBook<OpenAddressingIndex, LevelSearch::Hybrid>>;
TYPED_TEST_SUITE(Differential, FastTypes);

// Replays `bytes` into a reference book and a `Fast` book, comparing as it goes.
template <class Fast>
std::string run_differential(std::span<const std::uint8_t> bytes, std::uint64_t full_every,
                             ReferenceBook& ref, Fast& fast,
                             std::uint64_t max_messages = UINT64_MAX) {
    DifferentialHandler<ReferenceBook, Fast> diff(ref, fast, full_every);
    const auto r = itch::parse(bytes, diff, max_messages);
    if (r.status == itch::ParseStatus::BadLength) return "parse error at byte " + std::to_string(r.offset);
    if (auto e = diff.finish(); !e.empty()) return e;
    if (auto e = ref.validate(); !e.empty()) return "reference book invalid: " + e;
    if (auto e = fast.validate(); !e.empty()) return "fast book invalid: " + e;
    return {};
}

}  // namespace

TYPED_TEST(Differential, SyntheticStreamsMatchAfterEveryMessage) {
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        const auto bytes = generate_synthetic({.seed = seed, .messages = 200'000});

        ReferenceBook ref;
        TypeParam fast;
        EXPECT_EQ(run_differential(bytes, /*full_every=*/1, ref, fast), "");

        // The stream must actually exercise the hard cases, or this test proves little.
        const auto& a = ref.anomalies();
        EXPECT_GT(a.unknown_ref, 0u);
        EXPECT_GT(a.duplicate_ref, 0u);
        EXPECT_GT(a.over_reduce, 0u);
        EXPECT_GT(a.zero_share_add, 0u);
        EXPECT_GT(ref.order_count(), 1000u);
    }
}

TYPED_TEST(Differential, DeepBooksWithFewStocks) {
    // One stock, many far-away prices: hundreds of levels per side, so level
    // search and insertion in the middle of the vector get exercised hard.
    const auto bytes = generate_synthetic({.seed = 42, .messages = 300'000, .stocks = 1, .deep_rate = 0.5});
    ReferenceBook ref;
    TypeParam fast;
    EXPECT_EQ(run_differential(bytes, 1, ref, fast), "");
    EXPECT_GT(ref.depth(1, Side::Buy).size(), 200u);
}

TYPED_TEST(Differential, PoolAndIndexGrowBeyondInitialCapacity) {
    // Start tiny so the pool and the open-addressing table must grow mid-stream.
    const auto bytes = generate_synthetic({.seed = 7, .messages = 100'000});
    ReferenceBook ref;
    TypeParam fast(typename TypeParam::Config{.expected_orders = 4});
    EXPECT_EQ(run_differential(bytes, 1, ref, fast), "");
    EXPECT_GT(fast.pool_capacity(), 4u);
}

// The harness itself must catch a broken book, and say where.
struct DropsEvery500thCancel : FastBook {
    int n = 0;
    void on_order_cancel(const itch::OrderCancel& m) {
        if (++n % 500 != 0) FastBook::on_order_cancel(m);
    }
};

TEST(DifferentialHarness, CatchesABrokenBookAndReportsTheMessage) {
    const auto bytes = generate_synthetic({.seed = 3, .messages = 200'000});
    ReferenceBook ref;
    DropsEvery500thCancel broken;
    const std::string err = run_differential(bytes, 1, ref, broken);
    EXPECT_NE(err, "");
    EXPECT_NE(err.find("message #"), std::string::npos) << err;
    EXPECT_NE(err.find("type X"), std::string::npos) << err;
}

TEST(DifferentialRealData, NasdaqSamplePrefix) {
    const char* path = std::getenv("ITCH_SAMPLE");
    if (!path) GTEST_SKIP() << "set ITCH_SAMPLE=/path/to/NASDAQ_ITCH50 file to run";
    const char* n = std::getenv("ITCH_SAMPLE_MESSAGES");
    const std::uint64_t max_messages = n ? std::stoull(n) : 5'000'000;

    const MappedFile file(path);
    ReferenceBook ref;
    FastBook fast(FastBook::Config{.expected_orders = 1 << 22});
    EXPECT_EQ(run_differential(file.bytes(), 1000, ref, fast, max_messages), "");
    EXPECT_EQ(ref.anomalies().total(), 0u);
}
