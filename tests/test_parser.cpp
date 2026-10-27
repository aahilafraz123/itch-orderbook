#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ob/itch_parser.hpp"

using namespace ob::itch;
using Bytes = std::vector<std::uint8_t>;

namespace {

// Hand-written messages. The bytes are spelled out literally (not produced by an
// encoder) so the test can't share a bug with the decoder.
//
// Add Order: locate 42, tracking 7, 09:30:00.000000000, ref 1234567,
// BUY 100 AAPL @ $190.0500
const Bytes kAddAapl = {
    'A',                                             // 0  type
    0x00, 0x2A,                                      // 1  stock locate = 42
    0x00, 0x07,                                      // 3  tracking = 7
    0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,              // 5  timestamp = 34,200,000,000,000 ns
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87,  // 11 order ref = 1,234,567
    'B',                                             // 19 side
    0x00, 0x00, 0x00, 0x64,                          // 20 shares = 100
    'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',          // 24 stock
    0x00, 0x1C, 0xFF, 0xD4,                          // 32 price = 1,900,500 ($190.0500)
};

// Order Delete: locate 42, tracking 8, 1 ns after the add, ref 1234567
const Bytes kDeleteAapl = {
    'D',
    0x00, 0x2A,
    0x00, 0x08,
    0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87,
};

// Add Order with MPID: ref 1234568, SELL 250 MSFT @ $415.0000, attributed to "GSCO"
const Bytes kAddMsftMpid = {
    'F',
    0x00, 0x2A,
    0x00, 0x07,
    0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x88,  // 11 order ref = 1,234,568
    'S',                                             // 19 side
    0x00, 0x00, 0x00, 0xFA,                          // 20 shares = 250
    'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ',          // 24 stock
    0x00, 0x3F, 0x52, 0xF0,                          // 32 price = 4,150,000 ($415.0000)
    'G', 'S', 'C', 'O',                              // 36 attribution
};

// Order Executed: ref 1234567, 40 shares, match 987654321
const Bytes kExecAapl = {
    'E',
    0x00, 0x2A,
    0x00, 0x07,
    0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87,  // 11 order ref = 1,234,567
    0x00, 0x00, 0x00, 0x28,                          // 19 executed shares = 40
    0x00, 0x00, 0x00, 0x00, 0x3A, 0xDE, 0x68, 0xB1,  // 23 match number = 987,654,321
};

// Order Executed With Price: ref 1234567, 10 shares, match 987654322, printable,
// executed at $190.0400
const Bytes kExecPxAapl = {
    'C',
    0x00, 0x2A,
    0x00, 0x07,
    0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87,  // 11 order ref = 1,234,567
    0x00, 0x00, 0x00, 0x0A,                          // 19 executed shares = 10
    0x00, 0x00, 0x00, 0x00, 0x3A, 0xDE, 0x68, 0xB2,  // 23 match number = 987,654,322
    'Y',                                             // 31 printable
    0x00, 0x1C, 0xFF, 0x70,                          // 32 execution price = 1,900,400
};

// Order Cancel: ref 1234567, 25 shares canceled
const Bytes kCancelAapl = {
    'X',
    0x00, 0x2A,
    0x00, 0x07,
    0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87,  // 11 order ref = 1,234,567
    0x00, 0x00, 0x00, 0x19,                          // 19 canceled shares = 25
};

// Order Replace: ref 1234567 -> new ref 1234570, 300 shares @ $190.0600
const Bytes kReplaceAapl = {
    'U',
    0x00, 0x2A,
    0x00, 0x07,
    0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87,  // 11 original ref = 1,234,567
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x8A,  // 19 new ref = 1,234,570
    0x00, 0x00, 0x01, 0x2C,                          // 27 shares = 300
    0x00, 0x1D, 0x00, 0x38,                          // 31 price = 1,900,600 ($190.0600)
};

// System Event ('S', 12 bytes): a type we don't decode yet, so it must be skipped.
const Bytes kSystemEvent = {
    'S', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 'O',
};

// Prepend the 2-byte big-endian length that frames each message in the file.
Bytes frame(const Bytes& msg) {
    Bytes out = {static_cast<std::uint8_t>(msg.size() >> 8),
                 static_cast<std::uint8_t>(msg.size() & 0xFF)};
    out.insert(out.end(), msg.begin(), msg.end());
    return out;
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Records everything it's given, in order. Tests may allocate; the hot path won't.
struct RecordingHandler {
    std::vector<AddOrder>               adds;
    std::vector<OrderExecuted>          execs;
    std::vector<OrderExecutedWithPrice> execs_px;
    std::vector<OrderCancel>            cancels;
    std::vector<OrderDelete>            deletes;
    std::vector<OrderReplace>           replaces;
    std::vector<char>                   others;
    std::vector<char>                   order;  // type of every message, in arrival order

    void on_add_order(const AddOrder& m)                         { adds.push_back(m);     order.push_back(m.header.type); }
    void on_order_executed(const OrderExecuted& m)               { execs.push_back(m);    order.push_back(m.header.type); }
    void on_order_executed_with_price(const OrderExecutedWithPrice& m) { execs_px.push_back(m); order.push_back(m.header.type); }
    void on_order_cancel(const OrderCancel& m)                   { cancels.push_back(m);  order.push_back(m.header.type); }
    void on_order_delete(const OrderDelete& m)                   { deletes.push_back(m);  order.push_back(m.header.type); }
    void on_order_replace(const OrderReplace& m)                 { replaces.push_back(m); order.push_back(m.header.type); }
    void on_other(char t, const std::uint8_t*, std::size_t)      { others.push_back(t);   order.push_back(t); }

    std::size_t book_messages() const {
        return adds.size() + execs.size() + execs_px.size() + cancels.size() +
               deletes.size() + replaces.size();
    }
};

}  // namespace

// ---- Big-endian readers ---------------------------------------------------

TEST(ReadBigEndian, U16) {
    const std::uint8_t b[] = {0x12, 0x34};
    EXPECT_EQ(read_u16(b), 0x1234u);
}

TEST(ReadBigEndian, U32) {
    const std::uint8_t b[] = {0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(read_u32(b), 0x12345678u);
}

TEST(ReadBigEndian, U64) {
    const std::uint8_t b[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    EXPECT_EQ(read_u64(b), 0x0123456789ABCDEFull);
}

TEST(ReadBigEndian, U48) {
    const std::uint8_t b[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    EXPECT_EQ(read_u48(b), 0x0123456789ABull);
}

TEST(ReadBigEndian, HighBitsAreNotSignExtended) {
    const std::uint8_t b[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    EXPECT_EQ(read_u32(b), 0xFFFFFFFFu);
    EXPECT_EQ(read_u48(b), 0xFFFFFFFFFFFEull);
}

TEST(ReadBigEndian, WorksAtUnalignedAddress) {
    // Fields in ITCH sit at odd offsets (e.g. shares at 20, price at 32 after a
    // 2-byte length prefix). Reading at offset 1 must be correct and, under
    // -DOB_SANITIZE=ON, free of undefined behavior.
    const std::uint8_t b[] = {0xAA, 0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(read_u32(b + 1), 0x12345678u);
}

// ---- Message decoding -----------------------------------------------------

TEST(DecodeAddOrder, AllFields) {
    ASSERT_EQ(kAddAapl.size(), kAddOrderLen);
    const AddOrder m = decode_add_order(kAddAapl.data());

    EXPECT_EQ(m.header.type, 'A');
    EXPECT_EQ(m.header.locate, 42u);
    EXPECT_EQ(m.header.tracking, 7u);
    EXPECT_EQ(m.header.timestamp, 34'200'000'000'000ull);
    EXPECT_EQ(m.order_ref, 1'234'567ull);
    EXPECT_EQ(m.side, Side::Buy);
    EXPECT_EQ(m.shares, 100u);
    EXPECT_EQ(m.symbol(), "AAPL");
    EXPECT_EQ(m.price, 1'900'500u);
}

TEST(DecodeAddOrder, SellSide) {
    Bytes b = kAddAapl;
    b[19] = 'S';
    EXPECT_EQ(decode_add_order(b.data()).side, Side::Sell);
}

TEST(DecodeAddOrder, EightCharSymbolIsNotTrimmed) {
    Bytes b = kAddAapl;
    const char sym[] = "ABCDEFGH";
    for (std::size_t i = 0; i < 8; ++i) b[24 + i] = static_cast<std::uint8_t>(sym[i]);
    EXPECT_EQ(decode_add_order(b.data()).symbol(), "ABCDEFGH");
}

TEST(DecodeAddOrder, PlainAddHasBlankAttribution) {
    EXPECT_EQ(decode_add_order(kAddAapl.data()).mpid(), "");
}

TEST(DecodeAddOrderMpid, AllFields) {
    ASSERT_EQ(kAddMsftMpid.size(), kAddOrderMpidLen);
    const AddOrder m = decode_add_order_mpid(kAddMsftMpid.data());

    EXPECT_EQ(m.header.type, 'F');
    EXPECT_EQ(m.header.locate, 42u);
    EXPECT_EQ(m.order_ref, 1'234'568ull);
    EXPECT_EQ(m.side, Side::Sell);
    EXPECT_EQ(m.shares, 250u);
    EXPECT_EQ(m.symbol(), "MSFT");
    EXPECT_EQ(m.price, 4'150'000u);
    EXPECT_EQ(m.mpid(), "GSCO");
}

TEST(DecodeOrderExecuted, AllFields) {
    ASSERT_EQ(kExecAapl.size(), kOrderExecutedLen);
    const OrderExecuted m = decode_order_executed(kExecAapl.data());

    EXPECT_EQ(m.header.type, 'E');
    EXPECT_EQ(m.header.locate, 42u);
    EXPECT_EQ(m.header.timestamp, 34'200'000'000'000ull);
    EXPECT_EQ(m.order_ref, 1'234'567ull);
    EXPECT_EQ(m.executed_shares, 40u);
    EXPECT_EQ(m.match_number, 987'654'321ull);
}

TEST(DecodeOrderExecutedWithPrice, AllFields) {
    ASSERT_EQ(kExecPxAapl.size(), kOrderExecutedWithPriceLen);
    const OrderExecutedWithPrice m = decode_order_executed_with_price(kExecPxAapl.data());

    EXPECT_EQ(m.header.type, 'C');
    EXPECT_EQ(m.header.locate, 42u);
    EXPECT_EQ(m.order_ref, 1'234'567ull);
    EXPECT_EQ(m.executed_shares, 10u);
    EXPECT_EQ(m.match_number, 987'654'322ull);
    EXPECT_EQ(m.printable, 'Y');
    EXPECT_EQ(m.execution_price, 1'900'400u);
}

TEST(DecodeOrderExecutedWithPrice, NonPrintable) {
    Bytes b = kExecPxAapl;
    b[31] = 'N';
    EXPECT_EQ(decode_order_executed_with_price(b.data()).printable, 'N');
}

TEST(DecodeOrderCancel, AllFields) {
    ASSERT_EQ(kCancelAapl.size(), kOrderCancelLen);
    const OrderCancel m = decode_order_cancel(kCancelAapl.data());

    EXPECT_EQ(m.header.type, 'X');
    EXPECT_EQ(m.header.locate, 42u);
    EXPECT_EQ(m.order_ref, 1'234'567ull);
    EXPECT_EQ(m.canceled_shares, 25u);
}

TEST(DecodeOrderReplace, AllFields) {
    ASSERT_EQ(kReplaceAapl.size(), kOrderReplaceLen);
    const OrderReplace m = decode_order_replace(kReplaceAapl.data());

    EXPECT_EQ(m.header.type, 'U');
    EXPECT_EQ(m.header.locate, 42u);
    EXPECT_EQ(m.original_order_ref, 1'234'567ull);
    EXPECT_EQ(m.new_order_ref, 1'234'570ull);
    EXPECT_EQ(m.shares, 300u);
    EXPECT_EQ(m.price, 1'900'600u);
}

TEST(DecodeOrderDelete, AllFields) {
    ASSERT_EQ(kDeleteAapl.size(), kOrderDeleteLen);
    const OrderDelete m = decode_order_delete(kDeleteAapl.data());

    EXPECT_EQ(m.header.type, 'D');
    EXPECT_EQ(m.header.locate, 42u);
    EXPECT_EQ(m.header.tracking, 8u);
    EXPECT_EQ(m.header.timestamp, 34'200'000'000'001ull);
    EXPECT_EQ(m.order_ref, 1'234'567ull);
}

// ---- Stream framing -------------------------------------------------------

TEST(Parse, DispatchesInOrderAndSkipsUnknownTypes) {
    const Bytes buf = concat({frame(kSystemEvent), frame(kAddAapl), frame(kDeleteAapl)});
    RecordingHandler h;

    const ParseResult r = parse(buf, h);

    EXPECT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.messages, 3u);
    EXPECT_EQ(r.offset, buf.size());
    EXPECT_EQ(h.order, (std::vector<char>{'S', 'A', 'D'}));
    ASSERT_EQ(h.adds.size(), 1u);
    EXPECT_EQ(h.adds[0].order_ref, 1'234'567ull);
    ASSERT_EQ(h.deletes.size(), 1u);
    EXPECT_EQ(h.deletes[0].order_ref, 1'234'567ull);
}

TEST(Parse, RoutesEveryBookMessageType) {
    // A realistic life cycle: system event, add, attributed add, partial fill,
    // fill at a different price, partial cancel, replace, delete.
    const Bytes buf = concat({frame(kSystemEvent), frame(kAddAapl), frame(kAddMsftMpid),
                              frame(kExecAapl), frame(kExecPxAapl), frame(kCancelAapl),
                              frame(kReplaceAapl), frame(kDeleteAapl)});
    RecordingHandler h;

    const ParseResult r = parse(buf, h);

    EXPECT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.messages, 8u);
    EXPECT_EQ(r.offset, buf.size());
    EXPECT_EQ(h.order, (std::vector<char>{'S', 'A', 'F', 'E', 'C', 'X', 'U', 'D'}));
    ASSERT_EQ(h.adds.size(), 2u);  // 'A' and 'F' both arrive as AddOrder
    EXPECT_EQ(h.adds[1].mpid(), "GSCO");
    ASSERT_EQ(h.execs.size(), 1u);
    EXPECT_EQ(h.execs[0].executed_shares, 40u);
    ASSERT_EQ(h.execs_px.size(), 1u);
    EXPECT_EQ(h.execs_px[0].execution_price, 1'900'400u);
    ASSERT_EQ(h.cancels.size(), 1u);
    EXPECT_EQ(h.cancels[0].canceled_shares, 25u);
    ASSERT_EQ(h.replaces.size(), 1u);
    EXPECT_EQ(h.replaces[0].new_order_ref, 1'234'570ull);
    EXPECT_EQ(h.deletes.size(), 1u);
}

TEST(Parse, EveryBookTypeRejectsWrongLength) {
    // One byte short or one byte long must stop the parser before the handler
    // runs, for every type we decode.
    for (const Bytes* msg : {&kAddAapl, &kAddMsftMpid, &kExecAapl, &kExecPxAapl,
                             &kCancelAapl, &kDeleteAapl, &kReplaceAapl}) {
        SCOPED_TRACE(std::string("type ") + static_cast<char>((*msg)[0]));

        Bytes too_short(msg->begin(), msg->end() - 1);
        Bytes too_long = *msg;
        too_long.push_back(0x00);

        for (const Bytes* bad : {&too_short, &too_long}) {
            RecordingHandler h;
            const ParseResult r = parse(frame(*bad), h);
            EXPECT_EQ(r.status, ParseStatus::BadLength);
            EXPECT_EQ(r.messages, 0u);
            EXPECT_EQ(h.book_messages(), 0u);
        }
    }
}

TEST(Parse, EmptyBuffer) {
    RecordingHandler h;
    const ParseResult r = parse(std::span<const std::uint8_t>{}, h);
    EXPECT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.messages, 0u);
}

TEST(Parse, TruncatedMidMessage) {
    // Simulates cutting a big file at an arbitrary byte: the partial last
    // message must be reported, not read past the end of the buffer.
    Bytes buf = concat({frame(kAddAapl), frame(kDeleteAapl)});
    buf.pop_back();
    RecordingHandler h;

    const ParseResult r = parse(buf, h);

    EXPECT_EQ(r.status, ParseStatus::Truncated);
    EXPECT_EQ(r.messages, 1u);
    EXPECT_EQ(r.offset, 2 + kAddOrderLen);  // start of the incomplete delete
    EXPECT_TRUE(h.deletes.empty());
}

TEST(Parse, TruncatedMidLengthPrefix) {
    Bytes buf = frame(kAddAapl);
    buf.push_back(0x00);  // one byte of the next length prefix
    RecordingHandler h;

    const ParseResult r = parse(buf, h);

    EXPECT_EQ(r.status, ParseStatus::Truncated);
    EXPECT_EQ(r.messages, 1u);
}

TEST(Parse, WrongLengthForKnownTypeIsRejected) {
    // An 'A' framed as 35 bytes means our framing or the data is wrong.
    // Stop rather than decode garbage into the book.
    Bytes short_add(kAddAapl.begin(), kAddAapl.end() - 1);
    const Bytes buf = frame(short_add);
    RecordingHandler h;

    const ParseResult r = parse(buf, h);

    EXPECT_EQ(r.status, ParseStatus::BadLength);
    EXPECT_EQ(r.messages, 0u);
    EXPECT_EQ(r.offset, 0u);
    EXPECT_TRUE(h.adds.empty());
}

TEST(Parse, ZeroLengthIsRejected) {
    const Bytes buf = {0x00, 0x00};
    RecordingHandler h;
    EXPECT_EQ(parse(buf, h).status, ParseStatus::BadLength);
}

TEST(Parse, StopsAtMaxMessages) {
    const Bytes buf = concat({frame(kAddAapl), frame(kDeleteAapl), frame(kSystemEvent)});
    RecordingHandler h;

    const ParseResult r = parse(buf, h, /*max_messages=*/2);

    EXPECT_EQ(r.status, ParseStatus::Ok);
    EXPECT_EQ(r.messages, 2u);
    EXPECT_EQ(r.offset, 4 + kAddOrderLen + kOrderDeleteLen);
    EXPECT_EQ(h.order, (std::vector<char>{'A', 'D'}));
}
