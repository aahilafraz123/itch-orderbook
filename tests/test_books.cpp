#include <gtest/gtest.h>

#include <cstring>
#include <ostream>
#include <string_view>
#include <vector>

#include "ob/itch_parser.hpp"
#include "ob/fast_book.hpp"
#include "ob/reference_book.hpp"

using namespace ob;
using itch::Side;

static_assert(itch::Handler<ReferenceBook>, "books must plug straight into the parser");
static_assert(itch::Handler<FastBook>, "books must plug straight into the parser");

namespace ob {
// Readable gtest failure output for levels.
inline std::ostream& operator<<(std::ostream& os, const LevelSummary& l) {
    return os << "{price " << l.price << ", shares " << l.shares << ", orders " << l.orders << "}";
}
}  // namespace ob

namespace {

constexpr Locate kAapl = 1;
constexpr Locate kMsft = 2;

itch::Header header(char type, Locate locate) { return {type, locate, 0, 0}; }

itch::AddOrder add_msg(OrderRef ref, Side side, Shares shares, Price price,
                       Locate locate = kAapl, std::string_view sym = "AAPL") {
    itch::AddOrder m{};
    m.header    = header('A', locate);
    m.order_ref = ref;
    m.side      = side;
    m.shares    = shares;
    m.stock.fill(' ');
    std::memcpy(m.stock.data(), sym.data(), sym.size());
    m.price     = price;
    m.attribution.fill(' ');
    return m;
}

itch::OrderExecuted exec_msg(OrderRef ref, Shares n) { return {header('E', kAapl), ref, n, 0}; }

itch::OrderExecutedWithPrice exec_px_msg(OrderRef ref, Shares n, Price px) {
    return {header('C', kAapl), ref, n, 0, 'Y', px};
}

itch::OrderCancel cancel_msg(OrderRef ref, Shares n) { return {header('X', kAapl), ref, n}; }
itch::OrderDelete delete_msg(OrderRef ref) { return {header('D', kAapl), ref}; }

itch::OrderReplace replace_msg(OrderRef orig, OrderRef new_ref, Shares shares, Price price) {
    return {header('U', kAapl), orig, new_ref, shares, price};
}

using Levels = std::vector<LevelSummary>;
using Refs   = std::vector<OrderRef>;

// Every test runs against every book implementation: the reference book and
// each optimized variant the benchmark measures. Each gets a fresh book, and
// after every test the book's internal invariants are checked, so each test
// also proves the bookkeeping (level totals, queue links, index, free list)
// stayed consistent.
template <class Book>
class Books : public ::testing::Test {
protected:
    void TearDown() override { EXPECT_EQ(book.validate(), ""); }
    Book book;
};

using BookTypes = ::testing::Types<ReferenceBook,
                                   FastBook,
                                   BasicFastBook<StdIndex, LevelSearch::Linear>,
                                   BasicFastBook<OpenAddressingIndex, LevelSearch::Binary>,
                                   BasicFastBook<OpenAddressingIndex, LevelSearch::Hybrid>>;
TYPED_TEST_SUITE(Books, BookTypes);

}  // namespace

// ---- Adds and ordering ---------------------------------------------------

TYPED_TEST(Books, EmptyBookHasNoTopOfBook) {
    EXPECT_FALSE(this->book.best_bid(kAapl));
    EXPECT_FALSE(this->book.best_ask(kAapl));
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(Books, AddCreatesLevelOnCorrectSide) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_add_order(add_msg(2, Side::Sell, 200, 1'900'100));

    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'900'000, 100, 1}));
    EXPECT_EQ(this->book.best_ask(kAapl), (LevelSummary{1'900'100, 200, 1}));
    EXPECT_EQ(this->book.order_count(), 2u);
}

TYPED_TEST(Books, OrdersAtSamePriceAggregateInArrivalOrder) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_add_order(add_msg(2, Side::Buy, 300, 1'900'000));
    this->book.on_add_order(add_msg(3, Side::Buy, 50, 1'900'000));

    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'900'000, 450, 3}));
    EXPECT_EQ(this->book.queue(kAapl, Side::Buy, 1'900'000), (Refs{1, 2, 3}));
}

TYPED_TEST(Books, DepthIsBestFirstOnEachSide) {
    // Add out of order to prove the book sorts.
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'899'900));
    this->book.on_add_order(add_msg(2, Side::Buy, 100, 1'900'000));
    this->book.on_add_order(add_msg(3, Side::Buy, 100, 1'899'800));
    this->book.on_add_order(add_msg(4, Side::Sell, 100, 1'900'300));
    this->book.on_add_order(add_msg(5, Side::Sell, 100, 1'900'100));
    this->book.on_add_order(add_msg(6, Side::Sell, 100, 1'900'200));

    // Bids: highest first. Asks: lowest first.
    EXPECT_EQ(this->book.depth(kAapl, Side::Buy),
              (Levels{{1'900'000, 100, 1}, {1'899'900, 100, 1}, {1'899'800, 100, 1}}));
    EXPECT_EQ(this->book.depth(kAapl, Side::Sell),
              (Levels{{1'900'100, 100, 1}, {1'900'200, 100, 1}, {1'900'300, 100, 1}}));
    EXPECT_EQ(this->book.depth(kAapl, Side::Buy, 2).size(), 2u);
}

TYPED_TEST(Books, StocksAreIndependent) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000, kAapl, "AAPL"));
    this->book.on_add_order(add_msg(2, Side::Buy, 999, 4'150'000, kMsft, "MSFT"));

    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'900'000, 100, 1}));
    EXPECT_EQ(this->book.best_bid(kMsft), (LevelSummary{4'150'000, 999, 1}));
    EXPECT_EQ(this->book.symbol(kMsft), "MSFT");
    EXPECT_EQ(this->book.find_locate("MSFT"), kMsft);
    EXPECT_FALSE(this->book.find_locate("NOPE"));
}

TYPED_TEST(Books, AttributedAddBehavesLikePlainAdd) {
    itch::AddOrder m = add_msg(1, Side::Sell, 100, 1'900'100);
    m.header.type = 'F';
    std::memcpy(m.attribution.data(), "GSCO", 4);
    this->book.on_add_order(m);
    EXPECT_EQ(this->book.best_ask(kAapl), (LevelSummary{1'900'100, 100, 1}));
}

// ---- Executions and cancels ----------------------------------------------

TYPED_TEST(Books, PartialExecuteKeepsQueuePosition) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_add_order(add_msg(2, Side::Buy, 100, 1'900'000));
    this->book.on_order_executed(exec_msg(1, 40));

    EXPECT_EQ(this->book.find_order(1)->shares, 60u);
    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'900'000, 160, 2}));
    EXPECT_EQ(this->book.queue(kAapl, Side::Buy, 1'900'000), (Refs{1, 2}));  // still first
}

TYPED_TEST(Books, ExecutesAreCumulativeAndRemoveAtZero) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_order_executed(exec_msg(1, 30));
    this->book.on_order_executed(exec_msg(1, 30));
    this->book.on_order_executed(exec_msg(1, 40));

    EXPECT_EQ(this->book.find_order(1), nullptr);
    EXPECT_FALSE(this->book.best_bid(kAapl));  // level removed, not left at 0 shares
    EXPECT_EQ(this->book.anomalies().total(), 0u);
}

TYPED_TEST(Books, ExecuteWithPriceDoesNotMoveTheOrder) {
    this->book.on_add_order(add_msg(1, Side::Sell, 100, 1'900'100));
    this->book.on_order_executed_with_price(exec_px_msg(1, 25, 1'899'000));

    // Shares go down; the order stays at its own price, not the trade price.
    EXPECT_EQ(this->book.best_ask(kAapl), (LevelSummary{1'900'100, 75, 1}));
}

TYPED_TEST(Books, PartialCancelKeepsQueuePosition) {
    this->book.on_add_order(add_msg(1, Side::Sell, 100, 1'900'100));
    this->book.on_add_order(add_msg(2, Side::Sell, 100, 1'900'100));
    this->book.on_order_cancel(cancel_msg(1, 70));

    EXPECT_EQ(this->book.best_ask(kAapl), (LevelSummary{1'900'100, 130, 2}));
    EXPECT_EQ(this->book.queue(kAapl, Side::Sell, 1'900'100), (Refs{1, 2}));
}

TYPED_TEST(Books, CancelToZeroRemovesOrder) {
    this->book.on_add_order(add_msg(1, Side::Sell, 100, 1'900'100));
    this->book.on_order_cancel(cancel_msg(1, 100));
    EXPECT_EQ(this->book.find_order(1), nullptr);
    EXPECT_FALSE(this->book.best_ask(kAapl));
}

// ---- Deletes ----------------------------------------------------------------

TYPED_TEST(Books, DeleteFromMiddleOfQueueKeepsOthersInOrder) {
    for (OrderRef r = 1; r <= 3; ++r) this->book.on_add_order(add_msg(r, Side::Buy, 100, 1'900'000));
    this->book.on_order_delete(delete_msg(2));

    EXPECT_EQ(this->book.queue(kAapl, Side::Buy, 1'900'000), (Refs{1, 3}));
    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'900'000, 200, 2}));
}

TYPED_TEST(Books, DeletingLastOrderAtBestLevelExposesNextLevel) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_add_order(add_msg(2, Side::Buy, 500, 1'899'900));
    this->book.on_order_delete(delete_msg(1));

    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'899'900, 500, 1}));
    EXPECT_EQ(this->book.depth(kAapl, Side::Buy).size(), 1u);
}

// ---- Replaces ---------------------------------------------------------------

TYPED_TEST(Books, ReplaceChangingPriceMovesOrderAndRenamesIt) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_order_replace(replace_msg(1, 10, 300, 1'900'100));

    EXPECT_EQ(this->book.find_order(1), nullptr);
    const auto* o = this->book.find_order(10);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->side, Side::Buy);   // copied from the original
    EXPECT_EQ(o->locate, kAapl);     // copied from the original
    EXPECT_EQ(this->book.depth(kAapl, Side::Buy), (Levels{{1'900'100, 300, 1}}));
}

TYPED_TEST(Books, ReplaceAtSamePriceLosesQueuePriority) {
    for (OrderRef r = 1; r <= 3; ++r) this->book.on_add_order(add_msg(r, Side::Sell, 100, 1'900'100));
    // Same price, only the size changes. It still goes to the back.
    this->book.on_order_replace(replace_msg(1, 10, 50, 1'900'100));

    EXPECT_EQ(this->book.queue(kAapl, Side::Sell, 1'900'100), (Refs{2, 3, 10}));
    EXPECT_EQ(this->book.best_ask(kAapl), (LevelSummary{1'900'100, 250, 3}));
}

TYPED_TEST(Books, ReplacedOrderIsAddressedByNewRef) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_order_replace(replace_msg(1, 10, 100, 1'900'000));

    this->book.on_order_executed(exec_msg(1, 10));   // old ref: unknown now
    this->book.on_order_executed(exec_msg(10, 10));  // new ref: applies

    EXPECT_EQ(this->book.anomalies().unknown_ref, 1u);
    EXPECT_EQ(this->book.find_order(10)->shares, 90u);
}

// ---- Anomalies (documented policy: count, don't change the book) ------------

TYPED_TEST(Books, UnknownRefsAreCountedAndIgnored) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_order_executed(exec_msg(99, 10));
    this->book.on_order_executed_with_price(exec_px_msg(99, 10, 1));
    this->book.on_order_cancel(cancel_msg(99, 10));
    this->book.on_order_delete(delete_msg(99));
    this->book.on_order_replace(replace_msg(99, 100, 10, 1));

    EXPECT_EQ(this->book.anomalies().unknown_ref, 5u);
    EXPECT_EQ(this->book.order_count(), 1u);
    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'900'000, 100, 1}));
}

TYPED_TEST(Books, DuplicateAddRefIsCountedAndIgnored) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_add_order(add_msg(1, Side::Sell, 500, 1'950'000));

    EXPECT_EQ(this->book.anomalies().duplicate_ref, 1u);
    EXPECT_FALSE(this->book.best_ask(kAapl));
    EXPECT_EQ(this->book.best_bid(kAapl), (LevelSummary{1'900'000, 100, 1}));
}

TYPED_TEST(Books, ReplaceOntoLiveRefIsCountedAndIgnored) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_add_order(add_msg(2, Side::Buy, 100, 1'899'000));
    this->book.on_order_replace(replace_msg(1, 2, 300, 1'900'100));

    EXPECT_EQ(this->book.anomalies().duplicate_ref, 1u);
    EXPECT_EQ(this->book.order_count(), 2u);
    EXPECT_EQ(this->book.find_order(1)->price, 1'900'000u);  // original untouched
}

TYPED_TEST(Books, OverExecuteRemovesOrderAndIsCounted) {
    this->book.on_add_order(add_msg(1, Side::Buy, 100, 1'900'000));
    this->book.on_order_executed(exec_msg(1, 150));

    EXPECT_EQ(this->book.anomalies().over_reduce, 1u);
    EXPECT_EQ(this->book.find_order(1), nullptr);
    EXPECT_FALSE(this->book.best_bid(kAapl));
}

TYPED_TEST(Books, ZeroShareAndBadSideAddsAreCountedAndIgnored) {
    this->book.on_add_order(add_msg(1, Side::Buy, 0, 1'900'000));
    this->book.on_add_order(add_msg(2, static_cast<Side>('Q'), 100, 1'900'000));

    EXPECT_EQ(this->book.anomalies().zero_share_add, 1u);
    EXPECT_EQ(this->book.anomalies().bad_side, 1u);
    EXPECT_EQ(this->book.order_count(), 0u);
}

// ---- End to end through the parser -------------------------------------------

TYPED_TEST(Books, ReplaysFramedBytesFromTheParser) {
    // The literal Add Order fixture from test_parser.cpp: BUY 100 AAPL @ $190.05,
    // locate 42, ref 1234567, followed by an Order Delete of the same ref.
    const std::vector<std::uint8_t> buf = {
        0x00, 36,  // length prefix
        'A', 0x00, 0x2A, 0x00, 0x07, 0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87, 'B', 0x00, 0x00, 0x00, 0x64,
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', 0x00, 0x1C, 0xFF, 0xD4,
    };

    const auto r = itch::parse(buf, this->book);

    EXPECT_EQ(r.status, itch::ParseStatus::Ok);
    EXPECT_EQ(this->book.symbol(42), "AAPL");
    EXPECT_EQ(this->book.best_bid(42), (LevelSummary{1'900'500, 100, 1}));
}
