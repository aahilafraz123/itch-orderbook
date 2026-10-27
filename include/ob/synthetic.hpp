// Deterministic synthetic ITCH stream for differential tests and CI.
//
// Real NASDAQ files are gigabytes and not ours to redistribute, so CI uses
// this instead. It is built to hit the cases that break order books:
//   - a few hot price levels per stock, so queues get long and FIFO order matters
//   - occasional far-away prices, so books get deep (hundreds of levels)
//   - partial and full executions/cancels, executions at a different price
//   - replaces at the SAME price (must lose priority) and at new prices
//   - deletes from the middle of queues, emptying the best level
//   - a small rate of invalid messages (unknown refs, duplicate refs,
//     over-executions, zero-share adds) so the anomaly policy is compared too
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "ob/itch_writer.hpp"

namespace ob {

struct SyntheticOptions {
    std::uint64_t seed        = 1;
    std::uint64_t messages    = 100'000;
    Locate        stocks      = 8;     // locates 1..stocks
    std::uint32_t hot_levels  = 6;     // price levels each side that most orders use
    double        deep_rate   = 0.05;  // fraction of adds placed far from the inside
    double        invalid_rate = 0.02; // fraction of deliberately invalid messages
};

inline std::vector<std::uint8_t> generate_synthetic(const SyntheticOptions& opt) {
    struct Live {
        OrderRef ref;
        Locate   loc;
        Side     side;
        Price    price;
        Shares   shares;
    };
    std::mt19937_64 rng(opt.seed);
    auto uniform = [&](std::uint64_t lo, std::uint64_t hi) {  // inclusive
        return std::uniform_int_distribution<std::uint64_t>(lo, hi)(rng);
    };
    auto chance = [&](double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; };

    itch::Writer w;
    std::vector<Live> live;                          // all live orders
    std::unordered_map<OrderRef, std::size_t> pos;   // ref -> index in `live`
    OrderRef next_ref = 1;
    std::uint64_t match = 1;
    const Price tick = 100;  // $0.01

    auto symbol = [](Locate loc) { return "SYM" + std::to_string(loc); };
    auto mid = [](Locate loc) { return static_cast<Price>(1'000'000 + loc * 100'000); };  // $100 + $10*loc

    auto pick_price = [&](Locate loc, Side side) {
        const std::uint32_t off = chance(opt.deep_rate)
                                      ? static_cast<std::uint32_t>(uniform(opt.hot_levels, 300))
                                      : static_cast<std::uint32_t>(uniform(0, opt.hot_levels - 1));
        return side == Side::Buy ? mid(loc) - tick * (1 + off) : mid(loc) + tick * (1 + off);
    };

    auto remove_live = [&](std::size_t i) {
        pos.erase(live[i].ref);
        if (i != live.size() - 1) {
            live[i] = live.back();
            pos[live[i].ref] = i;
        }
        live.pop_back();
    };

    auto add_order = [&](Locate loc, Side side, Price price, Shares shares) {
        const OrderRef ref = next_ref++;
        w.add(loc, ref, side, shares, symbol(loc), price, chance(0.1) ? "MPID" : "");
        pos[ref] = live.size();
        live.push_back({ref, loc, side, price, shares});
    };

    w.system_event('O');
    while (w.messages() < opt.messages) {
        if (live.empty() || chance(0.38)) {
            const Locate loc = static_cast<Locate>(uniform(1, opt.stocks));
            const Side side = chance(0.5) ? Side::Buy : Side::Sell;
            add_order(loc, side, pick_price(loc, side), static_cast<Shares>(uniform(1, 10) * 100));
            continue;
        }

        if (chance(opt.invalid_rate)) {
            Live& any = live[uniform(0, live.size() - 1)];
            const OrderRef unknown = next_ref + 1'000'000'000ull;
            switch (uniform(0, 6)) {
                case 0: w.executed(any.loc, unknown, 100, match++); break;
                case 1: w.cancel(any.loc, unknown, 100); break;
                case 2: w.del(any.loc, unknown); break;
                case 3: w.replace(any.loc, unknown, next_ref++, 100, any.price); break;
                case 4: w.add(any.loc, any.ref, any.side, 100, symbol(any.loc), any.price); break;  // duplicate
                case 5: w.add(any.loc, next_ref++, Side::Buy, 0, symbol(any.loc), any.price); break;  // 0 shares
                case 6: {  // over-execution: the book removes the order
                    w.executed(any.loc, any.ref, any.shares + 1, match++);
                    remove_live(pos.at(any.ref));
                    break;
                }
            }
            continue;
        }

        const std::size_t i = uniform(0, live.size() - 1);
        Live& o = live[i];
        const std::uint64_t op = uniform(0, 99);
        if (op < 18) {  // execute
            const Shares n = chance(0.3) ? o.shares : static_cast<Shares>(uniform(1, o.shares));
            if (chance(0.2)) w.executed_with_price(o.loc, o.ref, n, match++, chance(0.9) ? 'Y' : 'N', o.price - 1);
            else             w.executed(o.loc, o.ref, n, match++);
            o.shares -= n;
            if (o.shares == 0) remove_live(i);
        } else if (op < 40) {  // partial or full cancel
            const Shares n = chance(0.25) ? o.shares : static_cast<Shares>(uniform(1, o.shares));
            w.cancel(o.loc, o.ref, n);
            o.shares -= n;
            if (o.shares == 0) remove_live(i);
        } else if (op < 75) {  // delete
            w.del(o.loc, o.ref);
            remove_live(i);
        } else {  // replace: same price (loses priority) or a new price, same side
            const Live old = o;
            const OrderRef new_ref = next_ref++;
            const Price price = chance(0.4) ? old.price : pick_price(old.loc, old.side);
            const Shares shares = static_cast<Shares>(uniform(1, 10) * 100);
            w.replace(old.loc, old.ref, new_ref, shares, price);
            remove_live(i);
            pos[new_ref] = live.size();
            live.push_back({new_ref, old.loc, old.side, price, shares});
        }
    }
    return std::move(w.bytes());
}

}  // namespace ob
