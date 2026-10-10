#include <gtest/gtest.h>

#include <random>

#include "pulse/portfolio/position.hpp"

using namespace pulse;
using namespace pulse::portfolio;

namespace {
constexpr Price D = kPriceScale;  // $1.00
}

TEST(Position, OpenAddAndCloseLong) {
  Position p;
  p.apply_fill(Side::Buy, 100 * D, 100);
  p.apply_fill(Side::Buy, 102 * D, 100);
  EXPECT_EQ(p.qty(), 200);
  EXPECT_EQ(p.avg_price(), 101 * D);
  EXPECT_EQ(p.unrealized(103 * D), 400 * D);
  p.apply_fill(Side::Sell, 105 * D, 50);
  EXPECT_EQ(p.realized(), 200 * D);  // (105 - 101) * 50
  EXPECT_EQ(p.avg_price(), 101 * D);
  p.apply_fill(Side::Sell, 99 * D, 150);
  EXPECT_EQ(p.qty(), 0);
  EXPECT_EQ(p.realized(), 200 * D - 300 * D);  // + (99 - 101) * 150
  EXPECT_EQ(p.open_cost(), 0);
}

TEST(Position, ShortSide) {
  Position p;
  p.apply_fill(Side::Sell, 50 * D, 10);
  EXPECT_EQ(p.qty(), -10);
  EXPECT_EQ(p.unrealized(48 * D), 20 * D);
  p.apply_fill(Side::Buy, 45 * D, 10);
  EXPECT_EQ(p.realized(), 50 * D);
  EXPECT_EQ(p.qty(), 0);
}

TEST(Position, FlipClosesThenOpensAtFillPrice) {
  Position p;
  p.apply_fill(Side::Buy, 10 * D, 100);
  p.apply_fill(Side::Sell, 12 * D, 250);  // close 100 (+200), open short 150 @ 12
  EXPECT_EQ(p.qty(), -150);
  EXPECT_EQ(p.realized(), 200 * D);
  EXPECT_EQ(p.avg_price(), 12 * D);
  EXPECT_EQ(p.open_cost(), 12 * D * 150);
}

// Property: however fills are split and ordered, once the position is flat
// the realized P&L equals total sell proceeds minus total buy cost -- no
// rounding drift from average-cost bookkeeping.
TEST(Position, FlatRealizedEqualsCashFlowUnderRandomFills) {
  std::mt19937_64 rng(3);
  for (int trial = 0; trial < 200; ++trial) {
    Position p;
    Money cash = 0;
    for (int i = 0; i < 200; ++i) {
      const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
      const Price px = (9000 + static_cast<Price>(rng() % 2000)) * 7;  // odd-ish prices
      const Qty q = static_cast<Qty>(1 + rng() % 333);
      p.apply_fill(s, px, q);
      cash += (s == Side::Sell ? 1 : -1) * px * static_cast<Money>(q);
    }
    // Flatten.
    const Price px = 70'000;
    if (p.qty() > 0) {
      cash += px * p.qty();
      p.apply_fill(Side::Sell, px, static_cast<Qty>(p.qty()));
    } else if (p.qty() < 0) {
      cash -= px * -p.qty();
      p.apply_fill(Side::Buy, px, static_cast<Qty>(-p.qty()));
    }
    ASSERT_EQ(p.qty(), 0);
    ASSERT_EQ(p.open_cost(), 0);
    ASSERT_EQ(p.realized(), cash) << "trial " << trial;
  }
}
