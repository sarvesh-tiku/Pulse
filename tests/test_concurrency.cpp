#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "pulse/concurrency/mutex_queue.hpp"
#include "pulse/concurrency/seqlock.hpp"
#include "pulse/concurrency/spsc_ring.hpp"
#include "pulse/market_data/event.hpp"
#include "pulse/telemetry/histogram.hpp"

using namespace pulse;
using namespace pulse::conc;

// Every queue must deliver every item exactly once, in order, across
// threads -- with a tiny capacity so the full/empty/wrap-around paths are hit
// constantly. Run under ThreadSanitizer in CI.
template <class Q>
class QueueStress : public ::testing::Test {};

using QueueTypes = ::testing::Types<SpscRing<MarketEvent>, SpscRing<MarketEvent, true, false>,
                                    SpscRing<MarketEvent, false, true>, MutexQueue<MarketEvent>>;
TYPED_TEST_SUITE(QueueStress, QueueTypes);

TYPED_TEST(QueueStress, FifoNoLossNoDuplication) {
  constexpr std::uint64_t kN = 300'000;
  TypeParam q(8);
  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kN; ++i) {
      MarketEvent e{};
      e.seq = i;
      e.order_id = i * 7;
      q.push(e);
    }
  });
  std::uint64_t expected = 1;
  bool ok = true;
  while (expected <= kN) {
    const auto n = q.pop_batch(
        [&](const MarketEvent& e) {
          ok &= e.seq == expected && e.order_id == expected * 7;
          ++expected;
        },
        5);
    if (n == 0) q.wait_nonempty(std::chrono::microseconds(100));
  }
  producer.join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(q.size_approx(), 0u);
}

TEST(SpscRing, TryPushFailsWhenFullAndCapacityRoundsUp) {
  SpscRing<int> q(5);
  EXPECT_EQ(q.capacity(), 8u);
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));
  int v = -1;
  EXPECT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 0);
  EXPECT_TRUE(q.try_push(8));
}

TEST(SeqLock, ReadersNeverSeeTornSnapshots) {
  struct Snap {
    std::uint64_t a, b, c, d, e, f, g, h;
  };
  SeqLock<Snap> lock;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (std::uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) lock.store({i, i, i, i, i, i, i, i});
  });
  std::uint64_t torn = 0, reads = 0, last = 0;
  bool monotonic = true;
  // Count-based, not time-based: sanitizers slow the reader down by 10-50x.
  // Readers must also overlap with writes, so require the version to advance.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (reads < 20'000 && std::chrono::steady_clock::now() < deadline) {
    const Snap s = lock.load();
    torn += !(s.a == s.b && s.b == s.c && s.c == s.d && s.d == s.e && s.e == s.f && s.f == s.g && s.g == s.h);
    monotonic &= s.a >= last;
    last = s.a;
    ++reads;
  }
  stop = true;
  writer.join();
  EXPECT_EQ(torn, 0u);
  EXPECT_TRUE(monotonic);
  EXPECT_EQ(reads, 20'000u);
  EXPECT_GT(last, 1000u);  // the writer really was writing concurrently
}

TEST(Histogram, ExactBelow128AndBoundedErrorAbove) {
  for (std::uint64_t v = 0; v < 128; ++v) EXPECT_EQ(telemetry::LatencyHistogram::value_of(telemetry::LatencyHistogram::index_of(v)), v);
  for (std::uint64_t v : {128ull, 1000ull, 12'345ull, 999'999ull, 123'456'789ull, 1ull << 40}) {
    const auto got = telemetry::LatencyHistogram::value_of(telemetry::LatencyHistogram::index_of(v));
    const double err = std::abs(static_cast<double>(got) - static_cast<double>(v)) / static_cast<double>(v);
    EXPECT_LT(err, 1.0 / 64) << v;
  }
  EXPECT_LT(telemetry::LatencyHistogram::index_of(~0ull), telemetry::LatencyHistogram::kBuckets);
}

TEST(Histogram, Percentiles) {
  telemetry::LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 100; ++v) h.record(v);
  EXPECT_EQ(h.percentile(50), 50u);
  EXPECT_EQ(h.percentile(99), 99u);
  EXPECT_EQ(h.percentile(100), 100u);
  EXPECT_EQ(h.min(), 1u);
  EXPECT_EQ(h.max(), 100u);
  telemetry::LatencyHistogram g;
  g.record(1'000'000);
  h.merge(g);
  EXPECT_EQ(h.count(), 101u);
  EXPECT_EQ(h.max(), 1'000'000u);
}
