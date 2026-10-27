// Two-thread pipeline pieces: the parser thread decodes messages into Events
// and pushes them into an SpscRing; the book thread pops and applies them.
#pragma once

#include <chrono>
#include <cstdint>

#include "ob/itch_messages.hpp"
#include "ob/spsc_ring.hpp"

namespace ob {

struct Event {
    enum class Kind : std::uint8_t { Add, Exec, ExecPx, Cancel, Delete, Replace, Other, End };

    Kind          kind;
    char          other_type;     // Kind::Other: the message type byte
    std::uint16_t other_len;      // Kind::Other: message length
    const std::uint8_t* other_bytes;  // Kind::Other: raw message (points into the mapped file)
    std::int64_t  stamp_ns;       // steady_clock time when the message became available
    union {
        itch::AddOrder               add;
        itch::OrderExecuted          exec;
        itch::OrderExecutedWithPrice exec_px;
        itch::OrderCancel            cancel;
        itch::OrderDelete            del;
        itch::OrderReplace           replace;
    };
};

inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// itch::Handler for the parser thread: wraps each decoded message in an Event
// and pushes it, spinning if the ring is full (back-pressure).
class RingProducer {
public:
    explicit RingProducer(SpscRing<Event>& ring) : ring_(ring) {}

    std::int64_t stamp_ns = 0;  // set by the caller before each dispatch

    void on_add_order(const itch::AddOrder& m)                       { Event e = make(Event::Kind::Add); e.add = m; push(e); }
    void on_order_executed(const itch::OrderExecuted& m)             { Event e = make(Event::Kind::Exec); e.exec = m; push(e); }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) { Event e = make(Event::Kind::ExecPx); e.exec_px = m; push(e); }
    void on_order_cancel(const itch::OrderCancel& m)                 { Event e = make(Event::Kind::Cancel); e.cancel = m; push(e); }
    void on_order_delete(const itch::OrderDelete& m)                 { Event e = make(Event::Kind::Delete); e.del = m; push(e); }
    void on_order_replace(const itch::OrderReplace& m)               { Event e = make(Event::Kind::Replace); e.replace = m; push(e); }
    void on_other(char type, const std::uint8_t* bytes, std::size_t len) {
        Event e = make(Event::Kind::Other);
        e.other_type  = type;
        e.other_len   = static_cast<std::uint16_t>(len);
        e.other_bytes = bytes;
        push(e);
    }

    void finish() { push(make(Event::Kind::End)); }
    std::uint64_t pushed() const { return pushed_; }

private:
    Event make(Event::Kind k) const {
        Event e{};
        e.kind = k;
        e.stamp_ns = stamp_ns;
        return e;
    }
    void push(const Event& e) {
        while (!ring_.try_push(e)) {}  // busy-spin: the consumer frees a slot within nanoseconds
        ++pushed_;
    }

    SpscRing<Event>& ring_;
    std::uint64_t    pushed_ = 0;
};

// Apply one event to a book. Returns false for Kind::End.
template <class Book>
bool apply_event(const Event& e, Book& book) {
    switch (e.kind) {
        case Event::Kind::Add:     book.on_add_order(e.add); break;
        case Event::Kind::Exec:    book.on_order_executed(e.exec); break;
        case Event::Kind::ExecPx:  book.on_order_executed_with_price(e.exec_px); break;
        case Event::Kind::Cancel:  book.on_order_cancel(e.cancel); break;
        case Event::Kind::Delete:  book.on_order_delete(e.del); break;
        case Event::Kind::Replace: book.on_order_replace(e.replace); break;
        case Event::Kind::Other:   book.on_other(e.other_type, e.other_bytes, e.other_len); break;
        case Event::Kind::End:     return false;
    }
    return true;
}

}  // namespace ob
