// Optimized order book.
//
// Same behavior as ReferenceBook (same queries, same anomaly policy, same time
// priority), different data structures:
//
//   Orders       one preallocated pool (std::vector<Order>) with a free list.
//                Removing an order puts its slot on the free list; the next add
//                reuses it. No allocation per order.
//   Queues       intrusive doubly linked list through the pool: each Order holds
//                the indices of its neighbors at the same price. O(1) unlink,
//                no separate list nodes. Indices (not pointers) so the pool can
//                grow without invalidating links.
//   Order lookup Index policy (OpenAddressingIndex by default; StdIndex for
//                comparison). See order_index.hpp.
//   Price levels per stock and side, a sorted std::vector<Level> with the BEST
//                price at the BACK. Most activity happens at or near the best
//                price, so inserts and erases there shift few elements, and
//                best_bid/best_ask is just back().
//                LevelSearch picks how a level is found: scanning from the back
//                (best) down, or binary search. Measured both; see README.
//
// Steady-state hot path allocates nothing: the pool and index are sized up
// front (Config::expected_orders), and a stock's level vector only grows the
// first few times that stock's book gets deeper than it has been before.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ob/book_types.hpp"
#include "ob/itch_messages.hpp"
#include "ob/order_index.hpp"

namespace ob {

// How a price level is located in a stock-side's sorted level vector:
//   Linear  scan from the best price outward. Fast when the level is near the
//           top (most messages), slow for orders deep in the book.
//   Binary  std::lower_bound over the whole vector. O(log n) always, but pays
//           for it even when the level is right at the top.
//   Hybrid  scan the kHybridScan levels nearest the best price, then fall back
//           to binary search over the rest. Added after perf showed ~15% of
//           remove() in the linear scan loop.
enum class LevelSearch { Linear, Binary, Hybrid };
inline constexpr std::size_t kHybridScan = 8;

template <class Index = OpenAddressingIndex, LevelSearch kSearch = LevelSearch::Linear>
class BasicFastBook {
public:
    // 32 bytes: two orders per 64-byte cache line.
    struct Order {
        OrderRef      ref;
        Price         price;
        Shares        shares;  // remaining, always > 0 while live
        std::uint32_t prev;    // pool index of the order ahead in the queue, or kNoSlot
        std::uint32_t next;    // pool index of the order behind (or next free slot)
        Locate        locate;
        Side          side;
    };
    static_assert(sizeof(Order) == 32);

    struct Config {
        std::size_t expected_orders = std::size_t{1} << 16;  // peak live orders to preallocate for
        std::size_t levels_reserve  = 0;                     // per stock-side, reserved on first use
    };

    BasicFastBook() : BasicFastBook(Config{}) {}

    explicit BasicFastBook(Config cfg)
        : cfg_(cfg), index_(cfg.expected_orders), sides_(kMaxLocates * 2) {
        // resize, not reserve: value-initializing the slots writes every page now,
        // so the OS maps the pool's memory here instead of page-faulting during
        // the trading day the first time each page is used.
        pool_.resize(cfg.expected_orders);
        std::array<char, 8> blank;
        blank.fill(' ');
        symbols_.assign(kMaxLocates, blank);
    }

    // ---- itch::Handler interface ----------------------------------------

    void on_add_order(const itch::AddOrder& m) {
        symbols_[m.header.locate] = m.stock;
        add(m.header.locate, m.order_ref, m.side, m.shares, m.price);
    }
    void on_order_executed(const itch::OrderExecuted& m) { reduce(m.order_ref, m.executed_shares); }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        reduce(m.order_ref, m.executed_shares);  // resting price unchanged, like 'E'
    }
    void on_order_cancel(const itch::OrderCancel& m) { reduce(m.order_ref, m.canceled_shares); }

    void on_order_delete(const itch::OrderDelete& m) {
        const std::uint32_t idx = index_.find(m.order_ref);
        if (idx == kNoSlot) { ++anomalies_.unknown_ref; return; }
        remove(idx);
    }

    void on_order_replace(const itch::OrderReplace& m) {
        const std::uint32_t idx = index_.find(m.original_order_ref);
        if (idx == kNoSlot) { ++anomalies_.unknown_ref; return; }
        if (m.new_order_ref != m.original_order_ref && index_.find(m.new_order_ref) != kNoSlot) {
            ++anomalies_.duplicate_ref;
            return;
        }
        const Locate locate = pool_[idx].locate;
        const Side   side   = pool_[idx].side;
        remove(idx);
        add(locate, m.new_order_ref, side, m.shares, m.price);  // back of the queue
    }

    // 'R' Stock Directory: NASDAQ announces every stock before trading starts.
    // Reserve that stock's level storage now, so the first orders of the day
    // don't pay for allocation. Every other non-book message is ignored.
    void on_other(char type, const std::uint8_t* msg, std::size_t len) {
        if (type != 'R' || len < 3 || cfg_.levels_reserve == 0) return;
        const Locate locate = itch::read_u16(msg + 1);
        for (Side side : {Side::Buy, Side::Sell}) {
            Levels& levels = side_levels(locate, side);
            if (levels.capacity() < cfg_.levels_reserve) levels.reserve(cfg_.levels_reserve);
        }
    }

    // ---- Queries (same contract as ReferenceBook) ---------------------------

    std::optional<LevelSummary> best_bid(Locate locate) const { return best(locate, Side::Buy); }
    std::optional<LevelSummary> best_ask(Locate locate) const { return best(locate, Side::Sell); }

    std::vector<LevelSummary> depth(Locate locate, Side side,
                                    std::size_t max_levels = std::numeric_limits<std::size_t>::max()) const {
        const auto& levels = side_levels(locate, side);
        std::vector<LevelSummary> out;
        for (std::size_t i = levels.size(); i-- > 0 && out.size() < max_levels;) {
            out.push_back(summarize(levels[i]));
        }
        return out;
    }

    std::vector<OrderRef> queue(Locate locate, Side side, Price price) const {
        std::vector<OrderRef> out;
        const auto& levels = side_levels(locate, side);
        const std::size_t li = find_level(levels, key_of(side, price));
        if (li == kNone) return out;
        for (std::uint32_t i = levels[li].head; i != kNoSlot; i = pool_[i].next) out.push_back(pool_[i].ref);
        return out;
    }

    const Order* find_order(OrderRef ref) const {
        const std::uint32_t idx = index_.find(ref);
        return idx == kNoSlot ? nullptr : &pool_[idx];
    }

    std::size_t order_count() const { return index_.size(); }
    const BookAnomalies& anomalies() const { return anomalies_; }
    std::size_t pool_capacity() const { return pool_.capacity(); }

    std::string_view symbol(Locate locate) const { return itch::trim_alpha(symbols_[locate]); }

    std::optional<Locate> find_locate(std::string_view sym) const {
        for (std::size_t i = 0; i < kMaxLocates; ++i) {
            if (symbol(static_cast<Locate>(i)) == sym) return static_cast<Locate>(i);
        }
        return std::nullopt;
    }

    // Full consistency check; "" if consistent. Slow: for tests and debugging.
    //  - each level vector strictly sorted by key (best at back), no empty levels
    //  - each queue's prev/next links agree, its tail is its last order, and its
    //    orders match the level's stock/side/price with shares > 0
    //  - level share total and order count match the queue
    //  - every queued order is findable through the index at its own slot
    //  - live orders + free-list slots == pool size (no leaked or double-freed slots)
    std::string validate() const {
        std::size_t queued = 0;
        for (std::size_t s = 0; s < sides_.size(); ++s) {
            const Locate loc  = static_cast<Locate>(s / 2);
            const Side   side = s % 2 == 0 ? Side::Buy : Side::Sell;
            const auto&  levels = sides_[s];
            for (std::size_t li = 0; li < levels.size(); ++li) {
                const Level& L = levels[li];
                const std::string where = "locate " + std::to_string(loc) + " " + static_cast<char>(side) +
                                          " @" + std::to_string(L.price);
                if (li > 0 && levels[li - 1].key >= L.key) return where + ": levels out of order";
                if (L.key != key_of(side, L.price)) return where + ": bad key";
                if (L.count == 0 || L.head == kNoSlot) return where + ": empty level";
                std::uint64_t sum = 0;
                std::uint32_t n = 0, prev = kNoSlot;
                for (std::uint32_t i = L.head; i != kNoSlot; prev = i, i = pool_[i].next) {
                    const Order& o = pool_[i];
                    const std::string ref = " ref " + std::to_string(o.ref);
                    if (o.prev != prev) return where + ref + ": broken prev link";
                    if (o.locate != loc || o.side != side || o.price != L.price) return where + ref + ": wrong level";
                    if (o.shares == 0) return where + ref + ": 0 shares";
                    if (index_.find(o.ref) != i) return where + ref + ": index points elsewhere";
                    sum += o.shares;
                    if (++n > pool_.size()) return where + ": cycle in queue";
                }
                if (prev != L.tail) return where + ": tail is not the last order";
                if (n != L.count) return where + ": count " + std::to_string(L.count) + " != " + std::to_string(n);
                if (sum != L.shares) return where + ": shares " + std::to_string(L.shares) + " != " + std::to_string(sum);
                queued += n;
            }
        }
        if (queued != index_.size()) {
            return std::to_string(index_.size()) + " indexed orders but " + std::to_string(queued) + " queued";
        }
        std::size_t free_slots = 0;
        for (std::uint32_t i = free_head_; i != kNoSlot; i = pool_[i].next) {
            if (++free_slots > pool_.size()) return "cycle in free list";
        }
        if (queued + free_slots != used_) {
            return "pool has handed out " + std::to_string(used_) + " slots but " + std::to_string(queued) +
                   " live + " + std::to_string(free_slots) + " free";
        }
        return {};
    }

private:
    // `key` orders both sides the same way: ascending, best at the back.
    // Bids: key = price (highest price = largest key). Asks: key = ~price
    // (lowest price = largest key). One sorted-vector implementation serves both.
    struct Level {
        std::uint32_t key;
        Price         price;
        std::uint64_t shares;
        std::uint32_t count;
        std::uint32_t head;  // pool index of the first order in the queue
        std::uint32_t tail;  // pool index of the last order (where adds go)
    };
    using Levels = std::vector<Level>;

    static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

    static std::uint32_t key_of(Side side, Price price) {
        return side == Side::Buy ? price : ~price;
    }

    static LevelSummary summarize(const Level& L) { return {L.price, L.shares, L.count}; }

    Levels& side_levels(Locate locate, Side side) {
        return sides_[std::size_t{locate} * 2 + (side == Side::Sell ? 1 : 0)];
    }
    const Levels& side_levels(Locate locate, Side side) const {
        return sides_[std::size_t{locate} * 2 + (side == Side::Sell ? 1 : 0)];
    }

    std::optional<LevelSummary> best(Locate locate, Side side) const {
        const auto& levels = side_levels(locate, side);
        if (levels.empty()) return std::nullopt;
        return summarize(levels.back());
    }

    // First index i with levels[i].key >= key, searching [0, end).
    static std::size_t lower_bound_key(const Levels& levels, std::size_t end, std::uint32_t key) {
        auto it = std::lower_bound(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(end), key,
                                   [](const Level& L, std::uint32_t k) { return L.key < k; });
        return static_cast<std::size_t>(it - levels.begin());
    }

    // Position where `key` is or would be inserted: first index whose key >= `key`.
    static std::size_t level_position(const Levels& levels, std::uint32_t key) {
        if constexpr (kSearch == LevelSearch::Binary) {
            return lower_bound_key(levels, levels.size(), key);
        } else {
            // Walk from the best price (back) outward while levels are better than `key`.
            std::size_t pos = levels.size();
            const std::size_t stop = kSearch == LevelSearch::Hybrid && pos > kHybridScan ? pos - kHybridScan : 0;
            while (pos > stop && levels[pos - 1].key > key) --pos;
            if (kSearch == LevelSearch::Hybrid && pos == stop && stop > 0) {
                return lower_bound_key(levels, stop, key);  // deeper than the scan window
            }
            if (pos > 0 && levels[pos - 1].key == key) return pos - 1;
            return pos;
        }
    }

    static std::size_t find_level(const Levels& levels, std::uint32_t key) {
        const std::size_t pos = level_position(levels, key);
        return pos < levels.size() && levels[pos].key == key ? pos : kNone;
    }

    // Index of the level for `key`, inserting an empty one at its sorted spot if needed.
    std::size_t find_or_insert_level(Levels& levels, std::uint32_t key, Price price) {
        const std::size_t pos = level_position(levels, key);
        if (pos < levels.size() && levels[pos].key == key) return pos;
        if (levels.capacity() == 0 && cfg_.levels_reserve != 0) levels.reserve(cfg_.levels_reserve);
        levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(pos), Level{key, price, 0, 0, kNoSlot, kNoSlot});
        return pos;
    }

    std::uint32_t alloc_slot() {
        if (free_head_ != kNoSlot) {
            const std::uint32_t idx = free_head_;
            free_head_ = pool_[idx].next;
            return idx;
        }
        if (used_ == pool_.size()) pool_.emplace_back();  // only past the preallocated size
        return used_++;
    }

    void free_slot(std::uint32_t idx) {
        pool_[idx].next = free_head_;
        free_head_ = idx;
    }

    void add(Locate locate, OrderRef ref, Side side, Shares shares, Price price) {
        if (side != Side::Buy && side != Side::Sell) { ++anomalies_.bad_side; return; }
        if (shares == 0) { ++anomalies_.zero_share_add; return; }
        if (index_.find(ref) != kNoSlot) { ++anomalies_.duplicate_ref; return; }

        const std::uint32_t idx = alloc_slot();
        Levels& levels = side_levels(locate, side);
        Level&  L = levels[find_or_insert_level(levels, key_of(side, price), price)];

        pool_[idx] = Order{ref, price, shares, L.tail, kNoSlot, locate, side};
        if (L.tail != kNoSlot) pool_[L.tail].next = idx;
        else                   L.head = idx;
        L.tail = idx;
        L.shares += shares;
        ++L.count;
        index_.insert(ref, idx);
    }

    void reduce(OrderRef ref, Shares n) {
        const std::uint32_t idx = index_.find(ref);
        if (idx == kNoSlot) { ++anomalies_.unknown_ref; return; }
        Order& o = pool_[idx];
        if (n > o.shares) { ++anomalies_.over_reduce; n = o.shares; }
        if (n == o.shares) { remove(idx); return; }

        o.shares -= n;
        Levels& levels = side_levels(o.locate, o.side);
        levels[find_level(levels, key_of(o.side, o.price))].shares -= n;
    }

    void remove(std::uint32_t idx) {
        const Order o = pool_[idx];
        Levels& levels = side_levels(o.locate, o.side);
        const std::size_t li = find_level(levels, key_of(o.side, o.price));
        Level& L = levels[li];

        if (o.prev != kNoSlot) pool_[o.prev].next = o.next;
        else                   L.head = o.next;
        if (o.next != kNoSlot) pool_[o.next].prev = o.prev;
        else                   L.tail = o.prev;
        L.shares -= o.shares;
        if (--L.count == 0) levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(li));

        index_.erase(o.ref);
        free_slot(idx);
    }

    Config                           cfg_;
    std::vector<Order>               pool_;
    std::uint32_t                    free_head_ = kNoSlot;
    std::uint32_t                    used_ = 0;  // slots ever handed out; [used_, size) never touched by orders
    Index                            index_;
    std::vector<Levels>              sides_;    // [locate * 2 + side]
    std::vector<std::array<char, 8>> symbols_;  // [locate]
    BookAnomalies                    anomalies_;
};

// The configuration we ship. Chosen by measurement (see README).
using FastBook = BasicFastBook<OpenAddressingIndex, LevelSearch::Linear>;

}  // namespace ob
