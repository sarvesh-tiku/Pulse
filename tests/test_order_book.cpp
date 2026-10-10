#include <gtest/gtest.h>

#include <array>
#include <random>

#include "pulse/orderbook/array_order_book.hpp"
#include "pulse/orderbook/map_order_book.hpp"

using namespace pulse;
using namespace pulse::book;

namespace {

// $100.00 reference, $0.01 tick, band 80.00 .. 120.00
InstrumentSpec spec() { return make_instrument(0, "TEST", 1'000'000); }
constexpr Price T = 100;          // one tick
constexpr Price P = 1'000'000;    // 100.00

template <class B>
B make_book() {
  return B(spec());
}

}  // namespace

template <class B>
class OrderBookTest : public ::testing::Test {
 protected:
  B book_ = make_book<B>();
};

using BookTypes = ::testing::Types<MapOrderBook, ArrayOrderBook<FlatIndex>, ArrayOrderBook<StdIndex>>;
TYPED_TEST_SUITE(OrderBookTest, BookTypes);

TYPED_TEST(OrderBookTest, EmptyBook) {
  EXPECT_EQ(this->book_.best_bid().price, kInvalidPrice);
  EXPECT_EQ(this->book_.best_ask().price, kInvalidPrice);
  EXPECT_EQ(this->book_.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, AddBuildsBothSides) {
  auto& b = this->book_;
  ASSERT_EQ(b.add(1, Side::Buy, P - T, 100), BookResult::Ok);
  ASSERT_EQ(b.add(2, Side::Buy, P - 2 * T, 200), BookResult::Ok);
  ASSERT_EQ(b.add(3, Side::Sell, P + T, 300), BookResult::Ok);
  ASSERT_EQ(b.add(4, Side::Buy, P - T, 50), BookResult::Ok);
  EXPECT_EQ(b.best_bid(), (Level{P - T, 150, 2}));
  EXPECT_EQ(b.best_ask(), (Level{P + T, 300, 1}));
  std::array<Level, 4> d{};
  ASSERT_EQ(b.depth(Side::Buy, d), 2u);
  EXPECT_EQ(d[0], (Level{P - T, 150, 2}));
  EXPECT_EQ(d[1], (Level{P - 2 * T, 200, 1}));
}

TYPED_TEST(OrderBookTest, RejectsInvalidAdds) {
  auto& b = this->book_;
  ASSERT_EQ(b.add(1, Side::Buy, P, 100), BookResult::Ok);
  EXPECT_EQ(b.add(1, Side::Buy, P, 100), BookResult::DuplicateOrderId);
  EXPECT_EQ(b.add(0, Side::Buy, P, 100), BookResult::DuplicateOrderId);
  EXPECT_EQ(b.add(2, Side::Buy, P, 0), BookResult::BadQty);
  EXPECT_EQ(b.add(3, Side::Buy, P + 1, 100), BookResult::BadPrice);            // off tick
  EXPECT_EQ(b.add(4, Side::Buy, spec().max_price + T, 100), BookResult::BadPrice);  // out of band
  EXPECT_EQ(b.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, BandEdgesAreUsable) {
  auto& b = this->book_;
  const auto s = spec();
  ASSERT_EQ(b.add(1, Side::Buy, s.min_price, 1), BookResult::Ok);
  ASSERT_EQ(b.add(2, Side::Sell, s.max_price, 1), BookResult::Ok);
  EXPECT_EQ(b.best_bid().price, s.min_price);
  EXPECT_EQ(b.best_ask().price, s.max_price);
  ASSERT_EQ(b.cancel(1), BookResult::Ok);
  ASSERT_EQ(b.cancel(2), BookResult::Ok);
  EXPECT_EQ(b.best_bid().price, kInvalidPrice);
  EXPECT_EQ(b.best_ask().price, kInvalidPrice);
}

TYPED_TEST(OrderBookTest, TimePriorityWithinLevel) {
  auto& b = this->book_;
  b.add(1, Side::Sell, P, 100);
  b.add(2, Side::Sell, P, 200);
  b.add(3, Side::Sell, P, 300);
  auto q = b.orders_at(Side::Sell, P);
  ASSERT_EQ(q.size(), 3u);
  EXPECT_EQ(q[0].id, 1u);
  EXPECT_EQ(q[1].id, 2u);
  EXPECT_EQ(q[2].id, 3u);
}

TYPED_TEST(OrderBookTest, ModifyDownKeepsPriorityOtherwiseLosesIt) {
  auto& b = this->book_;
  b.add(1, Side::Buy, P, 100);
  b.add(2, Side::Buy, P, 100);
  ASSERT_EQ(b.modify(1, P, 60), BookResult::Ok);  // reduce: stays first
  auto q = b.orders_at(Side::Buy, P);
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0].id, 1u);
  EXPECT_EQ(q[0].qty, 60u);
  EXPECT_EQ(b.best_bid().qty, 160u);

  ASSERT_EQ(b.modify(1, P, 80), BookResult::Ok);  // increase: goes to back
  q = b.orders_at(Side::Buy, P);
  EXPECT_EQ(q[0].id, 2u);
  EXPECT_EQ(q[1].id, 1u);
  EXPECT_EQ(b.best_bid().qty, 180u);

  ASSERT_EQ(b.modify(2, P + T, 100), BookResult::Ok);  // reprice: new level
  EXPECT_EQ(b.best_bid(), (Level{P + T, 100, 1}));
  EXPECT_EQ(b.modify(99, P, 1), BookResult::UnknownOrder);
  EXPECT_EQ(b.modify(1, P, 0), BookResult::BadQty);
}

TYPED_TEST(OrderBookTest, ExecutePartialThenFull) {
  auto& b = this->book_;
  b.add(1, Side::Sell, P, 100);
  b.add(2, Side::Sell, P + T, 100);
  ASSERT_EQ(b.execute(1, 40), BookResult::Ok);
  EXPECT_EQ(b.best_ask(), (Level{P, 60, 1}));
  EXPECT_EQ(b.execute(1, 61), BookResult::BadQty);
  ASSERT_EQ(b.execute(1, 60), BookResult::Ok);
  EXPECT_EQ(b.best_ask(), (Level{P + T, 100, 1}));
  EXPECT_FALSE(b.find(1));
  EXPECT_EQ(b.execute(1, 1), BookResult::UnknownOrder);
}

TYPED_TEST(OrderBookTest, BestLevelRecomputedAfterEmptying) {
  auto& b = this->book_;
  // Levels far apart to exercise multi-word bitmap scans in the array book.
  b.add(1, Side::Buy, P - 1000 * T, 10);
  b.add(2, Side::Buy, P - 3 * T, 10);
  b.add(3, Side::Buy, P, 10);
  b.add(4, Side::Sell, P + 700 * T, 10);
  b.add(5, Side::Sell, P + 64 * T, 10);
  b.cancel(3);
  EXPECT_EQ(b.best_bid().price, P - 3 * T);
  b.cancel(2);
  EXPECT_EQ(b.best_bid().price, P - 1000 * T);
  b.cancel(5);
  EXPECT_EQ(b.best_ask().price, P + 700 * T);
  std::array<Level, 3> d{};
  EXPECT_EQ(b.depth(Side::Sell, d), 1u);
}

// ---------------------------------------------------------------------------
// Differential property test: random operation streams applied to all three
// implementations must leave identical observable state after every step.
// MapOrderBook is simple enough to serve as the reference model.
// ---------------------------------------------------------------------------
class OrderBookDifferential : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(OrderBookDifferential, AllImplementationsAgree) {
  const auto s = spec();
  MapOrderBook ref(s);
  ArrayOrderBook<FlatIndex> fast(s, 16);  // tiny initial capacity: forces growth
  ArrayOrderBook<StdIndex> mid(s, 16);
  std::mt19937_64 rng(GetParam());
  std::vector<OrderId> ids;
  OrderId next = 1;

  auto rand_price = [&] {
    // Mostly near the touch; occasionally anywhere in (and just outside) the band.
    if (rng() % 20 == 0) return s.min_price - T + static_cast<Price>(rng() % (s.num_levels() + 2)) * T;
    return P + (static_cast<Price>(rng() % 41) - 20) * T;
  };

  for (int step = 0; step < 20'000; ++step) {
    const auto op = rng() % 10;
    BookResult r1{}, r2{}, r3{};
    if (op < 4 || ids.empty()) {
      const OrderId id = (rng() % 50 == 0 && !ids.empty()) ? ids[rng() % ids.size()] : next++;
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const Price px = rand_price();
      const Qty q = static_cast<Qty>(rng() % 5 == 0 ? 0 : 1 + rng() % 500);
      r1 = ref.add(id, side, px, q);
      r2 = fast.add(id, side, px, q);
      r3 = mid.add(id, side, px, q);
      if (r1 == BookResult::Ok) ids.push_back(id);
    } else {
      const std::size_t k = rng() % ids.size();
      const OrderId id = rng() % 30 == 0 ? next + 1000 : ids[k];
      if (op < 6) {
        r1 = ref.cancel(id);
        r2 = fast.cancel(id);
        r3 = mid.cancel(id);
      } else if (op < 8) {
        const Price px = rng() % 2 ? rand_price() : P;
        const Qty q = static_cast<Qty>(rng() % 400);
        r1 = ref.modify(id, px, q);
        r2 = fast.modify(id, px, q);
        r3 = mid.modify(id, px, q);
      } else {
        const Qty q = static_cast<Qty>(1 + rng() % 300);
        r1 = ref.execute(id, q);
        r2 = fast.execute(id, q);
        r3 = mid.execute(id, q);
      }
      if (!ref.find(ids[k])) {
        ids[k] = ids.back();
        ids.pop_back();
      }
    }
    ASSERT_EQ(r1, r2) << "step " << step;
    ASSERT_EQ(r1, r3) << "step " << step;
    ASSERT_EQ(ref.order_count(), fast.order_count()) << "step " << step;
    ASSERT_EQ(ref.best_bid(), fast.best_bid()) << "step " << step;
    ASSERT_EQ(ref.best_ask(), fast.best_ask()) << "step " << step;
    ASSERT_EQ(ref.best_bid(), mid.best_bid()) << "step " << step;
    if (step % 97 == 0) {
      for (Side side : {Side::Buy, Side::Sell}) {
        std::array<Level, 32> a{}, b{};
        const auto na = ref.depth(side, a);
        ASSERT_EQ(na, fast.depth(side, b));
        for (std::size_t i = 0; i < na; ++i) {
          ASSERT_EQ(a[i], b[i]);
          ASSERT_EQ(ref.orders_at(side, a[i].price).size(), fast.orders_at(side, a[i].price).size());
          const auto qa = ref.orders_at(side, a[i].price);
          const auto qb = fast.orders_at(side, a[i].price);
          for (std::size_t j = 0; j < qa.size(); ++j) {
            ASSERT_EQ(qa[j].id, qb[j].id);
            ASSERT_EQ(qa[j].qty, qb[j].qty);
          }
        }
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Seeds, OrderBookDifferential, ::testing::Range<std::uint64_t>(1, 9));
