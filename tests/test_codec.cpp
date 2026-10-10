#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <random>
#include <string_view>

#include "pulse/protocol/checksum.hpp"
#include "pulse/protocol/codec.hpp"

using namespace pulse;
using namespace pulse::protocol;

namespace {

MarketEvent make(EventType t) {
  MarketEvent e{};
  e.type = t;
  e.seq = 123456789012ull;
  e.exchange_ts = 34'200'000'000'123ull;
  e.instrument = 7;
  if (t != EventType::Heartbeat) e.order_id = 0xDEADBEEFCAFEull;
  if (t == EventType::AddOrder || t == EventType::ModifyOrder || t == EventType::Trade) {
    e.price = 1'945'100;
    e.qty = 300;
  }
  if (t == EventType::AddOrder || t == EventType::Trade) e.side = Side::Sell;
  return e;
}

bool same_wire_fields(const MarketEvent& a, const MarketEvent& b) {
  return a.type == b.type && a.seq == b.seq && a.exchange_ts == b.exchange_ts && a.instrument == b.instrument &&
         a.order_id == b.order_id && a.price == b.price && a.qty == b.qty && a.side == b.side;
}

constexpr EventType kAllTypes[] = {EventType::Heartbeat, EventType::AddOrder, EventType::CancelOrder,
                                   EventType::ModifyOrder, EventType::Trade};

}  // namespace

TEST(Checksum, KnownVector) {
  // Standard CRC32C check value for "123456789".
  constexpr std::string_view s = "123456789";
  std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(s.data()), s.size()};
  EXPECT_EQ(crc32c(0, bytes), 0xE3069283u);
  EXPECT_EQ(crc32c_software(0, bytes), 0xE3069283u);
}

TEST(Checksum, HardwareMatchesSoftware) {
  std::mt19937 rng(1);
  std::vector<std::byte> buf(257);
  for (auto& b : buf) b = static_cast<std::byte>(rng());
  for (std::size_t n = 0; n <= buf.size(); ++n) {
    std::span<const std::byte> s{buf.data(), n};
    ASSERT_EQ(crc32c(0, s), crc32c_software(0, s)) << n;
  }
}

TEST(Codec, RoundTripsEveryType) {
  for (auto t : kAllTypes) {
    std::array<std::byte, kMaxFrameSize> buf{};
    const MarketEvent in = make(t);
    const auto n = encode(in, buf);
    EXPECT_EQ(n, frame_size(t));
    EXPECT_EQ(n % 8, 0u);
    MarketEvent out{};
    const auto r = decode({buf.data(), n}, out);
    ASSERT_EQ(r.status, DecodeStatus::Ok) << to_string(t);
    EXPECT_EQ(r.consumed, n);
    EXPECT_TRUE(same_wire_fields(in, out)) << to_string(t);
  }
}

TEST(Codec, DetectsEverySingleBitFlipOutsideLength) {
  std::array<std::byte, kMaxFrameSize> buf{};
  const auto n = encode(make(EventType::AddOrder), buf);
  // Flips in the length field are a framing error, tested separately.
  for (std::size_t byte = 2; byte < n; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto copy = buf;
      copy[byte] ^= static_cast<std::byte>(1 << bit);
      MarketEvent out{};
      const auto r = decode({copy.data(), n}, out);
      EXPECT_NE(r.status, DecodeStatus::Ok) << "byte " << byte << " bit " << bit;
    }
  }
}

TEST(Codec, TruncatedFrameNeedsMoreData) {
  std::array<std::byte, kMaxFrameSize> buf{};
  const auto n = encode(make(EventType::Trade), buf);
  MarketEvent out{};
  EXPECT_EQ(decode({buf.data(), 10}, out).status, DecodeStatus::Truncated);
  const auto r = decode({buf.data(), n - 1}, out);
  EXPECT_EQ(r.status, DecodeStatus::Truncated);
  EXPECT_EQ(r.consumed, 0u);
}

TEST(Codec, BadLengthCannotResync) {
  std::array<std::byte, kMaxFrameSize> buf{};
  encode(make(EventType::CancelOrder), buf);
  const std::uint16_t bogus = 48;  // cancel frames are 40 bytes
  std::memcpy(buf.data(), &bogus, 2);
  MarketEvent out{};
  const auto r = decode(buf, out);
  EXPECT_EQ(r.status, DecodeStatus::BadLength);
  EXPECT_EQ(r.consumed, 0u);
}

TEST(Codec, UnknownTypeIsSkippableWhenLengthIsPlausible) {
  std::array<std::byte, kMaxFrameSize> buf{};
  const auto n = encode(make(EventType::AddOrder), buf);
  buf[off::type] = std::byte{99};
  MarketEvent out{};
  const auto r = decode({buf.data(), n}, out);
  EXPECT_EQ(r.status, DecodeStatus::UnknownType);
  EXPECT_EQ(r.consumed, n);
}

TEST(Codec, ChecksumCanBeDisabled) {
  std::array<std::byte, kMaxFrameSize> buf{};
  const auto n = encode(make(EventType::AddOrder), buf, /*with_checksum=*/false);
  MarketEvent out{};
  EXPECT_EQ(decode({buf.data(), n}, out).status, DecodeStatus::Ok);
  EXPECT_TRUE(out.flags & kFlagNoChecksum);
}

TEST(Codec, DecodesBackToBackFrames) {
  std::vector<std::byte> stream(kMaxFrameSize * 5);
  std::size_t pos = 0;
  for (auto t : kAllTypes) pos += encode(make(t), {stream.data() + pos, kMaxFrameSize});
  stream.resize(pos);
  std::size_t off = 0;
  int count = 0;
  MarketEvent out{};
  while (off < stream.size()) {
    const auto r = decode({stream.data() + off, stream.size() - off}, out);
    ASSERT_EQ(r.status, DecodeStatus::Ok);
    EXPECT_EQ(out.type, kAllTypes[count]);
    off += r.consumed;
    ++count;
  }
  EXPECT_EQ(count, 5);
}

TEST(Codec, RejectsBadSideAndReservedBits) {
  std::array<std::byte, kMaxFrameSize> buf{};
  const auto n = encode(make(EventType::AddOrder), buf);
  auto a = buf;
  a[off::body + 20] = std::byte{2};
  reseal({a.data(), n});
  MarketEvent out{};
  EXPECT_EQ(decode({a.data(), n}, out).status, DecodeStatus::BadField);
  auto b = buf;
  b[off::reserved] = std::byte{1};
  reseal({b.data(), n});
  EXPECT_EQ(decode({b.data(), n}, out).status, DecodeStatus::BadField);
}
