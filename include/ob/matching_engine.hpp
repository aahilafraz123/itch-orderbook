// Price-time priority matching engine for one instrument: the exchange's side.
//
// The order book elsewhere in this repo *reconstructs* NASDAQ's book from its
// feed. This is the other half: it accepts orders and decides the trades.
//
//   submit_limit   match against the opposite side while the price crosses,
//                  then rest any remainder on the book
//   submit_market  match against the opposite side at any price; any
//                  remainder is canceled (never rests)
//   cancel         remove a resting order
//
// Matching rules (the standard ones NASDAQ uses for its continuous book):
//   - Price priority: a buy takes the LOWEST ask first; a sell hits the
//     HIGHEST bid first.
//   - Time priority: within a price, the order that arrived first fills first.
//   - The trade happens at the RESTING (maker) order's price, so an aggressive
//     buy at $10.05 against an ask at $10.00 trades at $10.00 (price improvement).
//   - A partially filled resting order keeps its place in the queue.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ob/book_types.hpp"

namespace ob {

using OrderId = std::uint64_t;

struct Fill {
    OrderId taker;   // the incoming order
    OrderId maker;   // the resting order it traded against
    Side    taker_side;
    Price   price;   // always the maker's price
    Shares  shares;
    bool operator==(const Fill&) const = default;
};

enum class SubmitStatus {
    Accepted,      // filled, rested, or both
    DuplicateId,   // an order with this id is already resting
    InvalidOrder,  // zero shares or bad side
};

struct SubmitResult {
    SubmitStatus      status = SubmitStatus::Accepted;
    std::vector<Fill> fills;
    Shares            filled = 0;
    Shares            rested = 0;    // shares left resting on the book (limit orders)
    Shares            canceled = 0;  // unfilled shares of a market order
};

class MatchingEngine {
public:
    SubmitResult submit_limit(OrderId id, Side side, Price price, Shares shares) {
        SubmitResult r = validate(id, side, shares);
        if (r.status != SubmitStatus::Accepted) return r;
        Shares left = match(id, side, shares, price, r);
        if (left > 0) {
            rest(id, side, price, left);
            r.rested = left;
        }
        return r;
    }

    SubmitResult submit_market(OrderId id, Side side, Shares shares) {
        SubmitResult r = validate(id, side, shares);
        if (r.status != SubmitStatus::Accepted) return r;
        r.canceled = match(id, side, shares, std::nullopt, r);
        return r;
    }

    bool cancel(OrderId id) {
        auto it = resting_.find(id);
        if (it == resting_.end()) return false;
        const Resting& o = it->second;
        on_side(o.side, [&](auto& levels) {
            auto lvl = levels.find(o.price);
            lvl->second.erase(o.pos);
            if (lvl->second.empty()) levels.erase(lvl);
        });
        resting_.erase(it);
        return true;
    }

    std::optional<LevelSummary> best_bid() const { return top(bids_); }
    std::optional<LevelSummary> best_ask() const { return top(asks_); }

    std::vector<LevelSummary> depth(Side side) const {
        std::vector<LevelSummary> out;
        on_side(side, [&](const auto& levels) {
            for (const auto& [price, queue] : levels) out.push_back(summarize(price, queue));
        });
        return out;
    }

    std::vector<OrderId> queue(Side side, Price price) const {
        std::vector<OrderId> out;
        on_side(side, [&](const auto& levels) {
            auto lvl = levels.find(price);
            if (lvl == levels.end()) return;
            for (const auto& q : lvl->second) out.push_back(q.id);
        });
        return out;
    }

    std::size_t resting_count() const { return resting_.size(); }

private:
    struct Queued {
        OrderId id;
        Shares  shares;
    };
    using Queue = std::list<Queued>;
    struct Resting {
        Side            side;
        Price           price;
        Queue::iterator pos;
    };

    template <class Levels>
    static std::optional<LevelSummary> top(const Levels& levels) {
        if (levels.empty()) return std::nullopt;
        return summarize(levels.begin()->first, levels.begin()->second);
    }

    static LevelSummary summarize(Price price, const Queue& q) {
        std::uint64_t shares = 0;
        for (const auto& o : q) shares += o.shares;
        return {price, shares, static_cast<std::uint32_t>(q.size())};
    }

    template <class F>
    void on_side(Side side, F&& f) {
        if (side == Side::Buy) f(bids_);
        else                   f(asks_);
    }
    template <class F>
    void on_side(Side side, F&& f) const {
        if (side == Side::Buy) f(bids_);
        else                   f(asks_);
    }

    SubmitResult validate(OrderId id, Side side, Shares shares) const {
        SubmitResult r;
        if (shares == 0 || (side != Side::Buy && side != Side::Sell)) r.status = SubmitStatus::InvalidOrder;
        else if (resting_.contains(id)) r.status = SubmitStatus::DuplicateId;
        return r;
    }

    // Match `shares` of an incoming order against the opposite side. `limit`
    // empty = market order (any price). Returns the unfilled remainder.
    Shares match(OrderId id, Side side, Shares shares, std::optional<Price> limit, SubmitResult& r) {
        auto sweep = [&](auto& opposite, auto crosses) {
            while (shares > 0 && !opposite.empty()) {
                auto lvl = opposite.begin();  // best opposite price
                if (limit && !crosses(lvl->first, *limit)) break;
                Queue& q = lvl->second;
                while (shares > 0 && !q.empty()) {
                    Queued& maker = q.front();  // earliest order at this price
                    const Shares n = std::min(shares, maker.shares);
                    r.fills.push_back({id, maker.id, side, lvl->first, n});
                    r.filled += n;
                    shares -= n;
                    maker.shares -= n;
                    if (maker.shares == 0) {
                        resting_.erase(maker.id);
                        q.pop_front();
                    }
                }
                if (q.empty()) opposite.erase(lvl);
            }
        };
        if (side == Side::Buy) sweep(asks_, [](Price ask, Price lim) { return ask <= lim; });
        else                   sweep(bids_, [](Price bid, Price lim) { return bid >= lim; });
        return shares;
    }

    void rest(OrderId id, Side side, Price price, Shares shares) {
        on_side(side, [&](auto& levels) {
            Queue& q = levels[price];
            q.push_back({id, shares});
            resting_.emplace(id, Resting{side, price, std::prev(q.end())});
        });
    }

    std::map<Price, Queue, std::greater<Price>> bids_;  // best (highest) first
    std::map<Price, Queue, std::less<Price>>    asks_;  // best (lowest) first
    std::unordered_map<OrderId, Resting>        resting_;
};

}  // namespace ob
