// Reference order book: slow, simple, obviously correct.
//
// This is the ground truth the optimized book gets checked against, so every
// choice here favors "you can verify it by reading it" over speed. It uses
// standard containers, allocates freely, and keeps full time priority (the FIFO
// queue of orders at each price) so the differential test can catch queue bugs,
// not just wrong totals.
//
// It satisfies itch::Handler, so it plugs straight into the parser:
//     ob::ReferenceBook book;
//     ob::itch::parse(bytes, book);
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ob/itch_messages.hpp"

namespace ob {

using itch::Side;
using OrderRef = std::uint64_t;
using Price    = std::uint32_t;  // 4 implied decimals, never floating point
using Shares   = std::uint32_t;
using Locate   = std::uint16_t;  // per-day stock ID, 0..65535

// One price level as seen from outside the book.
struct LevelSummary {
    Price         price;
    std::uint64_t shares;  // sum over the level's orders; 64-bit so it can't overflow
    std::uint32_t orders;

    bool operator==(const LevelSummary&) const = default;
};

// Messages that don't make sense against the current book. On a full-day file
// replayed from the start, every counter should stay 0; if one doesn't, either
// the data or our logic is wrong. Policy: count it, leave the book unchanged
// (except over_reduce, which removes the order since it can't have shares left).
struct BookAnomalies {
    std::uint64_t unknown_ref    = 0;  // E/C/X/D/U for an order ref we don't have
    std::uint64_t duplicate_ref  = 0;  // A/F/U adding a ref that's already live
    std::uint64_t over_reduce    = 0;  // E/C/X taking more shares than remain
    std::uint64_t zero_share_add = 0;  // A/F/U adding an order with 0 shares
    std::uint64_t bad_side       = 0;  // side byte not 'B' or 'S'

    std::uint64_t total() const {
        return unknown_ref + duplicate_ref + over_reduce + zero_share_add + bad_side;
    }
};

class ReferenceBook {
public:
    struct Order {
        Locate locate;
        Side   side;
        Price  price;
        Shares shares;  // remaining, always > 0 while the order is live
        // Where this order sits in its level's queue. std::list iterators stay
        // valid when other elements are inserted or erased, so we can remove
        // an order from the middle of the queue in O(1) without searching.
        std::list<OrderRef>::iterator queue_pos;
    };

    static constexpr std::size_t kMaxLocates = std::size_t{1} << 16;

    ReferenceBook() : stocks_(kMaxLocates), symbols_(kMaxLocates, blank_symbol()) {}

    // ---- itch::Handler interface ----------------------------------------

    void on_add_order(const itch::AddOrder& m) {  // 'A' and 'F'
        symbols_[m.header.locate] = m.stock;
        add(m.header.locate, m.order_ref, m.side, m.shares, m.price);
    }

    void on_order_executed(const itch::OrderExecuted& m) {
        reduce(m.order_ref, m.executed_shares);
    }

    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        // The trade printed at execution_price, but the resting order's price
        // doesn't change. To the book this is exactly an 'E'.
        reduce(m.order_ref, m.executed_shares);
    }

    void on_order_cancel(const itch::OrderCancel& m) {
        reduce(m.order_ref, m.canceled_shares);
    }

    void on_order_delete(const itch::OrderDelete& m) {
        auto it = orders_.find(m.order_ref);
        if (it == orders_.end()) { ++anomalies_.unknown_ref; return; }
        remove(it);
    }

    void on_order_replace(const itch::OrderReplace& m) {
        auto it = orders_.find(m.original_order_ref);
        if (it == orders_.end()) { ++anomalies_.unknown_ref; return; }
        if (m.new_order_ref != m.original_order_ref && orders_.contains(m.new_order_ref)) {
            ++anomalies_.duplicate_ref;
            return;
        }
        // Side and stock can't change on a replace, so they aren't in the
        // message. Copy them from the original before it's removed.
        const Locate locate = it->second.locate;
        const Side   side   = it->second.side;
        remove(it);
        // A brand-new order: it joins the back of the queue at its price, even
        // if the price didn't change. That's the "loses priority" rule.
        add(locate, m.new_order_ref, side, m.shares, m.price);
    }

    void on_other(char, const std::uint8_t*, std::size_t) {}  // doesn't touch the book

    // ---- Queries ----------------------------------------------------------

    std::optional<LevelSummary> best_bid(Locate locate) const { return best(locate, Side::Buy); }
    std::optional<LevelSummary> best_ask(Locate locate) const { return best(locate, Side::Sell); }

    // Levels from best to worst: highest bid first, lowest ask first.
    std::vector<LevelSummary> depth(Locate locate, Side side,
                                    std::size_t max_levels = std::numeric_limits<std::size_t>::max()) const {
        std::vector<LevelSummary> out;
        on_side(stocks_[locate], side, [&](const auto& levels) {
            for (const auto& [price, level] : levels) {
                if (out.size() == max_levels) break;
                out.push_back(summarize(price, level));
            }
        });
        return out;
    }

    // Order refs at one price, front of the queue (next to trade) first.
    std::vector<OrderRef> queue(Locate locate, Side side, Price price) const {
        std::vector<OrderRef> out;
        on_side(stocks_[locate], side, [&](const auto& levels) {
            auto lvl = levels.find(price);
            if (lvl != levels.end()) out.assign(lvl->second.queue.begin(), lvl->second.queue.end());
        });
        return out;
    }

    const Order* find_order(OrderRef ref) const {
        auto it = orders_.find(ref);
        return it == orders_.end() ? nullptr : &it->second;
    }

    std::size_t order_count() const { return orders_.size(); }
    const BookAnomalies& anomalies() const { return anomalies_; }

    // Symbol for a locate, learned from Add Order messages ("" if none seen yet).
    std::string_view symbol(Locate locate) const { return itch::trim_alpha(symbols_[locate]); }

    std::optional<Locate> find_locate(std::string_view sym) const {
        for (std::size_t i = 0; i < kMaxLocates; ++i) {
            if (symbol(static_cast<Locate>(i)) == sym) return static_cast<Locate>(i);
        }
        return std::nullopt;
    }

    // Full consistency check. Returns "" if the book is consistent, otherwise
    // a description of the first problem found. Slow: for tests and debugging.
    //  - no empty levels
    //  - each level's share total == sum of its orders' shares
    //  - every queued ref is a live order whose stock/side/price match the level,
    //    with shares > 0 and a queue_pos pointing back at that exact queue slot
    //  - every live order is queued exactly once
    std::string validate() const {
        std::size_t queued = 0;
        for (std::size_t loc = 0; loc < kMaxLocates; ++loc) {
            for (Side side : {Side::Buy, Side::Sell}) {
                std::string err;
                on_side(stocks_[loc], side, [&](const auto& levels) {
                    for (const auto& [price, level] : levels) {
                        const std::string where = "locate " + std::to_string(loc) + " " +
                                                  static_cast<char>(side) + " @" + std::to_string(price);
                        if (level.queue.empty()) { err = where + ": empty level"; return; }
                        std::uint64_t sum = 0;
                        for (const OrderRef& ref : level.queue) {
                            auto it = orders_.find(ref);
                            if (it == orders_.end()) { err = where + ": queued ref " + std::to_string(ref) + " not live"; return; }
                            const Order& o = it->second;
                            if (o.locate != loc || o.side != side || o.price != price) {
                                err = where + ": ref " + std::to_string(ref) + " is in the wrong level";
                                return;
                            }
                            if (o.shares == 0) { err = where + ": ref " + std::to_string(ref) + " has 0 shares"; return; }
                            if (&*o.queue_pos != &ref) { err = where + ": ref " + std::to_string(ref) + " has a stale queue_pos"; return; }
                            sum += o.shares;
                            ++queued;
                        }
                        if (sum != level.shares) {
                            err = where + ": level total " + std::to_string(level.shares) +
                                  " != sum of orders " + std::to_string(sum);
                            return;
                        }
                    }
                });
                if (!err.empty()) return err;
            }
        }
        if (queued != orders_.size()) {
            return std::to_string(orders_.size()) + " live orders but " + std::to_string(queued) + " queued";
        }
        return {};
    }

private:
    struct Level {
        std::uint64_t       shares = 0;  // sum of the queued orders' remaining shares
        std::list<OrderRef> queue;       // FIFO: front is first in line
    };

    // std::map keeps prices sorted, so begin() is always the best price:
    // bids sorted high-to-low, asks low-to-high.
    struct StockBook {
        std::map<Price, Level, std::greater<Price>> bids;
        std::map<Price, Level, std::less<Price>>    asks;
    };

    using OrderMap = std::unordered_map<OrderRef, Order>;

    // bids and asks are different map types (different sort order), so this
    // calls `f` with whichever one `side` names. Works for const and non-const.
    template <class Book, class F>
    static void on_side(Book& book, Side side, F&& f) {
        if (side == Side::Buy) f(book.bids);
        else                   f(book.asks);
    }

    static LevelSummary summarize(Price price, const Level& level) {
        return {price, level.shares, static_cast<std::uint32_t>(level.queue.size())};
    }

    static std::array<char, 8> blank_symbol() {
        std::array<char, 8> s;
        s.fill(' ');
        return s;
    }

    std::optional<LevelSummary> best(Locate locate, Side side) const {
        auto top = depth(locate, side, 1);
        if (top.empty()) return std::nullopt;
        return top.front();
    }

    void add(Locate locate, OrderRef ref, Side side, Shares shares, Price price) {
        if (side != Side::Buy && side != Side::Sell) { ++anomalies_.bad_side; return; }
        if (shares == 0) { ++anomalies_.zero_share_add; return; }
        if (orders_.contains(ref)) { ++anomalies_.duplicate_ref; return; }

        on_side(stocks_[locate], side, [&](auto& levels) {
            Level& level = levels[price];  // creates the level if this price is new
            level.queue.push_back(ref);    // back of the line: time priority
            level.shares += shares;
            orders_.emplace(ref, Order{locate, side, price, shares, std::prev(level.queue.end())});
        });
    }

    // E, C and X all just take shares away. The order keeps its place in line.
    void reduce(OrderRef ref, Shares n) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++anomalies_.unknown_ref; return; }
        Order& o = it->second;
        if (n > o.shares) { ++anomalies_.over_reduce; n = o.shares; }
        if (n == o.shares) { remove(it); return; }  // nothing left: the order is dead

        o.shares -= n;
        on_side(stocks_[o.locate], o.side, [&](auto& levels) { levels.at(o.price).shares -= n; });
    }

    void remove(OrderMap::iterator it) {
        const Order& o = it->second;
        on_side(stocks_[o.locate], o.side, [&](auto& levels) {
            auto lvl = levels.find(o.price);
            lvl->second.queue.erase(o.queue_pos);
            lvl->second.shares -= o.shares;
            if (lvl->second.queue.empty()) levels.erase(lvl);  // never leave empty levels
        });
        orders_.erase(it);
    }

    std::vector<StockBook>           stocks_;   // indexed by locate
    std::vector<std::array<char, 8>> symbols_;  // indexed by locate
    OrderMap                         orders_;   // every live order, by ref
    BookAnomalies                    anomalies_;
};

}  // namespace ob
