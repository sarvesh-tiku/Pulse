#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "pulse/market_data/generator.hpp"
#include "pulse/orderbook/map_order_book.hpp"
#include "pulse/protocol/codec.hpp"

using namespace pulse;
using namespace pulse::md;

#ifndef PULSE_GENERATOR_GOLDEN
#define PULSE_GENERATOR_GOLDEN 965312165710578310ull  // recorded on macOS arm64, verified on Linux x86-64 + arm64
#endif

namespace {

// Applies the stream to an independent reference book (the std::map one, not
// the array book the generator uses internally) and checks venue invariants.
void check_stream_valid(WorkloadProfile profile, std::uint64_t seed) {
  GeneratorConfig cfg;
  cfg.events = 60'000;
  cfg.instruments = 4;
  cfg.seed = seed;
  cfg.profile = profile;
  const auto uni = InstrumentUniverse::synthetic(cfg.instruments);
  FeedGenerator gen(cfg, uni);
  std::vector<book::MapOrderBook> books;
  for (const auto& s : uni.specs()) books.emplace_back(s);

  MarketEvent ev;
  SeqNum expect_seq = 1;
  TimestampNs last_ts = 0;
  std::array<std::uint64_t, 5> counts{};
  while (gen.next(ev)) {
    ASSERT_GE(ev.exchange_ts, last_ts);
    last_ts = ev.exchange_ts;
    if (ev.type == EventType::Heartbeat) {
      EXPECT_EQ(ev.seq, expect_seq);  // announces the next sequence
      continue;
    }
    ASSERT_EQ(ev.seq, expect_seq++);
    ++counts[static_cast<std::size_t>(ev.type)];
    auto& b = books[ev.instrument];
    book::BookResult r{};
    switch (ev.type) {
      case EventType::AddOrder: r = b.add(ev.order_id, ev.side, ev.price, ev.qty); break;
      case EventType::CancelOrder: r = b.cancel(ev.order_id); break;
      case EventType::ModifyOrder: r = b.modify(ev.order_id, ev.price, ev.qty); break;
      case EventType::Trade: {
        book::OrderView o;
        ASSERT_TRUE(b.find(ev.order_id, o)) << "trade against unknown order";
        EXPECT_EQ(o.price, ev.price);
        EXPECT_EQ(o.side, ev.side);
        // Trades always hit the front of the best level (price-time priority).
        const auto best = ev.side == Side::Buy ? b.best_bid() : b.best_ask();
        EXPECT_EQ(best.price, ev.price);
        EXPECT_EQ(b.orders_at(ev.side, ev.price).front().id, ev.order_id);
        r = b.execute(ev.order_id, ev.qty);
        break;
      }
      default: FAIL();
    }
    ASSERT_EQ(r, book::BookResult::Ok) << to_string(ev.type) << " seq " << ev.seq;
    const auto bid = b.best_bid(), ask = b.best_ask();
    if (bid.price != kInvalidPrice && ask.price != kInvalidPrice) {
      ASSERT_LT(bid.price, ask.price) << "crossed book at seq " << ev.seq;
    }
  }
  EXPECT_EQ(gen.produced(), cfg.events);
  for (std::size_t t = 1; t <= 4; ++t) EXPECT_GT(counts[t], 0u) << "type " << t << " never generated";
}

}  // namespace

TEST(Generator, BalancedStreamIsValid) { check_stream_valid(WorkloadProfile::Balanced, 1); }
TEST(Generator, AddHeavyStreamIsValid) { check_stream_valid(WorkloadProfile::AddHeavy, 2); }
TEST(Generator, CancelHeavyStreamIsValid) { check_stream_valid(WorkloadProfile::CancelHeavy, 3); }
TEST(Generator, TradeHeavyStreamIsValid) { check_stream_valid(WorkloadProfile::TradeHeavy, 4); }
TEST(Generator, BurstStreamIsValid) { check_stream_valid(WorkloadProfile::Burst, 5); }

TEST(Generator, SameSeedSameBytes) {
  GeneratorConfig cfg;
  cfg.events = 5'000;
  const auto uni = InstrumentUniverse::synthetic(3);
  FeedGenerator a(cfg, uni), b(cfg, uni);
  MarketEvent ea, eb;
  std::array<std::byte, protocol::kMaxFrameSize> ba{}, bb{};
  while (a.next(ea)) {
    ASSERT_TRUE(b.next(eb));
    const auto na = protocol::encode(ea, ba);
    const auto nb = protocol::encode(eb, bb);
    ASSERT_EQ(na, nb);
    ASSERT_EQ(0, std::memcmp(ba.data(), bb.data(), na));
  }
  EXPECT_FALSE(b.next(eb));
}

// Golden value: the first events of seed 42 must be identical on every
// platform/compiler (the distributions are implemented in core/random.hpp,
// not taken from the standard library). If this changes, sessions recorded
// elsewhere no longer reproduce.
TEST(Generator, CrossPlatformGoldenPrefix) {
  GeneratorConfig cfg;
  cfg.events = 2'000;
  cfg.instruments = 4;
  cfg.seed = 42;
  FeedGenerator g(cfg, InstrumentUniverse::synthetic(4));
  MarketEvent ev;
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  while (g.next(ev)) {
    mix(ev.seq);
    mix(ev.exchange_ts);
    mix(ev.order_id);
    mix(static_cast<std::uint64_t>(ev.price));
    mix(ev.qty);
    mix(static_cast<std::uint64_t>(ev.type));
  }
  RecordProperty("golden", std::to_string(h));
  EXPECT_EQ(h, PULSE_GENERATOR_GOLDEN);
}
