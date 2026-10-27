#include <gtest/gtest.h>

#include <cstdint>
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
    std::vector<AddOrder>    adds;
    std::vector<OrderDelete> deletes;
    std::vector<char>        others;
    std::vector<char>        order;  // type of every message, in arrival order

    void on_add_order(const AddOrder& m)        { adds.push_back(m);    order.push_back('A'); }
    void on_order_delete(const OrderDelete& m)  { deletes.push_back(m); order.push_back('D'); }
    void on_other(char t, const std::uint8_t*, std::size_t) { others.push_back(t); order.push_back(t); }
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
