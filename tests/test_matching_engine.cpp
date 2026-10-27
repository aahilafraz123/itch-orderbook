#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "ob/matching_engine.hpp"

using namespace ob;

namespace ob {
inline std::ostream& operator<<(std::ostream& os, const Fill& f) {
    return os << "{taker " << f.taker << ", maker " << f.maker << ", price " << f.price << ", shares " << f.shares
              << "}";
}
inline std::ostream& operator<<(std::ostream& os, const LevelSummary& l) {
    return os << "{price " << l.price << ", shares " << l.shares << ", orders " << l.orders << "}";
}
}  // namespace ob

using Fills  = std::vector<Fill>;
using Levels = std::vector<LevelSummary>;
using Ids    = std::vector<OrderId>;

TEST(MatchingEngine, NonCrossingOrdersRest) {
    MatchingEngine m;
    auto r1 = m.submit_limit(1, Side::Buy, 100'000, 10);
    auto r2 = m.submit_limit(2, Side::Sell, 100'100, 20);

    EXPECT_TRUE(r1.fills.empty());
    EXPECT_EQ(r1.rested, 10u);
    EXPECT_TRUE(r2.fills.empty());
    EXPECT_EQ(m.best_bid(), (LevelSummary{100'000, 10, 1}));
    EXPECT_EQ(m.best_ask(), (LevelSummary{100'100, 20, 1}));
}

TEST(MatchingEngine, CrossingLimitTradesAtMakerPrice) {
    MatchingEngine m;
    m.submit_limit(1, Side::Sell, 100'000, 10);
    auto r = m.submit_limit(2, Side::Buy, 100'500, 10);  // willing to pay more

    EXPECT_EQ(r.fills, (Fills{{2, 1, Side::Buy, 100'000, 10}}));  // trades at the ask
    EXPECT_EQ(r.rested, 0u);
    EXPECT_FALSE(m.best_ask());
    EXPECT_FALSE(m.best_bid());
}

TEST(MatchingEngine, PricePriorityBeforeTimePriority) {
    MatchingEngine m;
    m.submit_limit(1, Side::Sell, 100'200, 10);  // arrived first, worse price
    m.submit_limit(2, Side::Sell, 100'100, 10);  // arrived second, better price
    auto r = m.submit_limit(3, Side::Buy, 100'200, 10);

    EXPECT_EQ(r.fills, (Fills{{3, 2, Side::Buy, 100'100, 10}}));
}

TEST(MatchingEngine, TimePriorityWithinAPrice) {
    MatchingEngine m;
    m.submit_limit(1, Side::Buy, 100'000, 10);
    m.submit_limit(2, Side::Buy, 100'000, 10);
    m.submit_limit(3, Side::Buy, 100'000, 10);
    auto r = m.submit_limit(4, Side::Sell, 100'000, 15);

    EXPECT_EQ(r.fills, (Fills{{4, 1, Side::Sell, 100'000, 10}, {4, 2, Side::Sell, 100'000, 5}}));
    // Order 2 was partially filled and keeps its place at the front.
    EXPECT_EQ(m.queue(Side::Buy, 100'000), (Ids{2, 3}));
    EXPECT_EQ(m.best_bid(), (LevelSummary{100'000, 15, 2}));
}

TEST(MatchingEngine, SweepsMultipleLevelsThenRestsRemainder) {
    MatchingEngine m;
    m.submit_limit(1, Side::Sell, 100'000, 5);
    m.submit_limit(2, Side::Sell, 100'100, 5);
    m.submit_limit(3, Side::Sell, 100'300, 5);  // beyond the buyer's limit
    auto r = m.submit_limit(4, Side::Buy, 100'200, 20);

    EXPECT_EQ(r.fills, (Fills{{4, 1, Side::Buy, 100'000, 5}, {4, 2, Side::Buy, 100'100, 5}}));
    EXPECT_EQ(r.filled, 10u);
    EXPECT_EQ(r.rested, 10u);
    EXPECT_EQ(m.best_bid(), (LevelSummary{100'200, 10, 1}));
    EXPECT_EQ(m.best_ask(), (LevelSummary{100'300, 5, 1}));
}

TEST(MatchingEngine, MarketOrderSweepsAnyPriceAndNeverRests) {
    MatchingEngine m;
    m.submit_limit(1, Side::Buy, 100'000, 5);
    m.submit_limit(2, Side::Buy, 90'000, 5);
    auto r = m.submit_market(3, Side::Sell, 20);

    EXPECT_EQ(r.fills, (Fills{{3, 1, Side::Sell, 100'000, 5}, {3, 2, Side::Sell, 90'000, 5}}));
    EXPECT_EQ(r.canceled, 10u);
    EXPECT_EQ(r.rested, 0u);
    EXPECT_FALSE(m.best_bid());
    EXPECT_FALSE(m.best_ask());  // remainder did not rest
}

TEST(MatchingEngine, MarketOrderIntoEmptyBook) {
    MatchingEngine m;
    auto r = m.submit_market(1, Side::Buy, 100);
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.canceled, 100u);
}

TEST(MatchingEngine, CancelRemovesRestingOrder) {
    MatchingEngine m;
    m.submit_limit(1, Side::Sell, 100'000, 10);
    m.submit_limit(2, Side::Sell, 100'000, 10);

    EXPECT_TRUE(m.cancel(1));
    EXPECT_FALSE(m.cancel(1));   // already gone
    EXPECT_FALSE(m.cancel(99));  // never existed
    EXPECT_EQ(m.queue(Side::Sell, 100'000), (Ids{2}));

    auto r = m.submit_market(3, Side::Buy, 10);
    EXPECT_EQ(r.fills, (Fills{{3, 2, Side::Buy, 100'000, 10}}));  // canceled order can't trade
}

TEST(MatchingEngine, FullyFilledMakerCannotBeCanceled) {
    MatchingEngine m;
    m.submit_limit(1, Side::Sell, 100'000, 10);
    m.submit_market(2, Side::Buy, 10);
    EXPECT_FALSE(m.cancel(1));
    EXPECT_EQ(m.resting_count(), 0u);
}

TEST(MatchingEngine, RejectsInvalidAndDuplicateOrders) {
    MatchingEngine m;
    EXPECT_EQ(m.submit_limit(1, Side::Buy, 100'000, 0).status, SubmitStatus::InvalidOrder);
    EXPECT_EQ(m.submit_limit(1, static_cast<Side>('Q'), 100'000, 10).status, SubmitStatus::InvalidOrder);
    EXPECT_EQ(m.submit_limit(1, Side::Buy, 100'000, 10).status, SubmitStatus::Accepted);
    EXPECT_EQ(m.submit_limit(1, Side::Sell, 200'000, 10).status, SubmitStatus::DuplicateId);
    EXPECT_EQ(m.resting_count(), 1u);
}

TEST(MatchingEngine, RandomizedInvariants) {
    // Thousands of random orders. After every step: the book is never crossed,
    // every fill is at the maker's price and within the taker's limit, and
    // shares are conserved (submitted == filled + rested + canceled).
    MatchingEngine m;
    std::mt19937_64 rng(12345);
    auto pick = [&](std::uint64_t lo, std::uint64_t hi) { return std::uniform_int_distribution<std::uint64_t>(lo, hi)(rng); };
    std::vector<OrderId> live;

    for (OrderId id = 1; id <= 20'000; ++id) {
        const Side side = pick(0, 1) ? Side::Buy : Side::Sell;
        const Shares shares = static_cast<Shares>(pick(1, 500));
        const std::uint64_t action = pick(0, 9);
        if (action == 0 && !live.empty()) {
            m.cancel(live[pick(0, live.size() - 1)]);
            continue;
        }
        SubmitResult r;
        std::optional<Price> limit;
        if (action == 1) {
            r = m.submit_market(id, side, shares);
        } else {
            limit = static_cast<Price>(100'000 + pick(0, 40) * 100);
            r = m.submit_limit(id, side, *limit, shares);
        }
        ASSERT_EQ(r.status, SubmitStatus::Accepted);
        ASSERT_EQ(r.filled + r.rested + r.canceled, shares);
        Shares sum = 0;
        for (const Fill& f : r.fills) {
            sum += f.shares;
            if (limit) {
                if (side == Side::Buy) ASSERT_LE(f.price, *limit);
                else                   ASSERT_GE(f.price, *limit);
            }
        }
        ASSERT_EQ(sum, r.filled);
        if (r.rested) live.push_back(id);
        const auto bid = m.best_bid();
        const auto ask = m.best_ask();
        if (bid && ask) ASSERT_LT(bid->price, ask->price) << "book crossed after order " << id;
    }
}
