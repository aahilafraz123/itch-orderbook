// Differential testing: replay one message stream into two books and prove
// they agree.
//
// The reference book is simple enough to trust by reading it. If the
// optimized book produces the same answers after every message of a real
// trading day, that's much stronger evidence than any set of hand-written
// cases: hundreds of millions of real situations, including ones nobody
// thought to test.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ob/book_types.hpp"
#include "ob/itch_messages.hpp"

namespace ob {

inline std::string describe(const LevelSummary& l) {
    return "{price " + std::to_string(l.price) + ", shares " + std::to_string(l.shares) + ", orders " +
           std::to_string(l.orders) + "}";
}

// Compare one stock's full depth on both sides (and optionally every queue).
// Returns "" if identical, else the first difference.
template <class A, class B>
std::string compare_stock(const A& a, const B& b, Locate loc, bool queues) {
    for (Side side : {Side::Buy, Side::Sell}) {
        const auto da = a.depth(loc, side);
        const auto db = b.depth(loc, side);
        const std::string where = "locate " + std::to_string(loc) + " " + static_cast<char>(side);
        for (std::size_t i = 0; i < std::max(da.size(), db.size()); ++i) {
            if (i >= da.size()) return where + " level " + std::to_string(i) + ": only B has " + describe(db[i]);
            if (i >= db.size()) return where + " level " + std::to_string(i) + ": only A has " + describe(da[i]);
            if (!(da[i] == db[i])) {
                return where + " level " + std::to_string(i) + ": A " + describe(da[i]) + " vs B " + describe(db[i]);
            }
            if (queues && a.queue(loc, side, da[i].price) != b.queue(loc, side, da[i].price)) {
                return where + " @" + std::to_string(da[i].price) + ": queue order differs";
            }
        }
    }
    return {};
}

template <class A, class B>
std::string compare_totals(const A& a, const B& b) {
    if (a.order_count() != b.order_count()) {
        return "live orders: A " + std::to_string(a.order_count()) + " vs B " + std::to_string(b.order_count());
    }
    if (!(a.anomalies() == b.anomalies())) {
        return "anomaly counters differ (A total " + std::to_string(a.anomalies().total()) + ", B total " +
               std::to_string(b.anomalies().total()) + ")";
    }
    return {};
}

// Everything: every stock, every level, every queue, totals and anomalies.
template <class A, class B>
std::string compare_books(const A& a, const B& b) {
    if (auto e = compare_totals(a, b); !e.empty()) return e;
    for (std::size_t loc = 0; loc < kMaxLocates; ++loc) {
        if (auto e = compare_stock(a, b, static_cast<Locate>(loc), true); !e.empty()) return e;
    }
    return {};
}

// An itch::Handler that feeds every message to both books.
//  - after EVERY message: the touched stock's best bid and best ask must match
//  - every `full_every` messages: full depth of every stock touched since the
//    last check, plus live-order totals and anomaly counters
//  - at the end (finish()): everything, including every queue's FIFO order
// On the first mismatch it records the message index, type and order ref and
// stops forwarding.
template <class A, class B>
class DifferentialHandler {
public:
    DifferentialHandler(A& a, B& b, std::uint64_t full_every)
        : a_(a), b_(b), full_every_(full_every ? full_every : 1), dirty_flag_(kMaxLocates, false) {}

    void on_add_order(const itch::AddOrder& m) { step(m.header, m.order_ref, [&](auto& bk) { bk.on_add_order(m); }); }
    void on_order_executed(const itch::OrderExecuted& m) {
        step(m.header, m.order_ref, [&](auto& bk) { bk.on_order_executed(m); });
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        step(m.header, m.order_ref, [&](auto& bk) { bk.on_order_executed_with_price(m); });
    }
    void on_order_cancel(const itch::OrderCancel& m) {
        step(m.header, m.order_ref, [&](auto& bk) { bk.on_order_cancel(m); });
    }
    void on_order_delete(const itch::OrderDelete& m) {
        step(m.header, m.order_ref, [&](auto& bk) { bk.on_order_delete(m); });
    }
    void on_order_replace(const itch::OrderReplace& m) {
        step(m.header, m.original_order_ref, [&](auto& bk) { bk.on_order_replace(m); });
    }
    void on_other(char, const std::uint8_t*, std::size_t) { ++index_; }

    // Run the final full comparison. Returns "" if the books agree everywhere.
    std::string finish() {
        if (!error_.empty()) return error_;
        if (auto e = compare_books(a_, b_); !e.empty()) error_ = "at end of stream: " + e;
        return error_;
    }

    const std::string& error() const { return error_; }
    std::uint64_t      messages() const { return index_; }
    std::uint64_t      book_messages() const { return book_messages_; }
    std::uint64_t      full_checks() const { return full_checks_; }

private:
    template <class Apply>
    void step(const itch::Header& h, OrderRef ref, Apply&& apply) {
        const std::uint64_t i = index_++;
        if (!error_.empty()) return;
        ++book_messages_;
        apply(a_);
        apply(b_);

        const Locate loc = h.locate;
        if (!dirty_flag_[loc]) { dirty_flag_[loc] = true; dirty_.push_back(loc); }

        std::string e;
        if (a_.best_bid(loc) != b_.best_bid(loc)) e = "best bid differs";
        else if (a_.best_ask(loc) != b_.best_ask(loc)) e = "best ask differs";
        else if (index_ % full_every_ == 0) e = full_check();
        if (!e.empty()) {
            error_ = "message #" + std::to_string(i) + " (type " + h.type + ", locate " + std::to_string(loc) +
                     ", order ref " + std::to_string(ref) + "): " + e;
        }
    }

    std::string full_check() {
        ++full_checks_;
        if (auto e = compare_totals(a_, b_); !e.empty()) return e;
        for (Locate loc : dirty_) {
            dirty_flag_[loc] = false;
            if (auto e = compare_stock(a_, b_, loc, false); !e.empty()) return e;
        }
        dirty_.clear();
        return {};
    }

    A&                  a_;
    B&                  b_;
    std::uint64_t       full_every_;
    std::uint64_t       index_ = 0;
    std::uint64_t       full_checks_ = 0;
    std::uint64_t       book_messages_ = 0;
    std::vector<bool>   dirty_flag_;
    std::vector<Locate> dirty_;
    std::string         error_;
};

}  // namespace ob
