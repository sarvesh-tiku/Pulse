#include <gtest/gtest.h>

#include "pulse/risk/risk_engine.hpp"

using namespace pulse;
using namespace pulse::risk;

namespace {

exec::OrderRequest buy(Price px, Qty q) {
  exec::OrderRequest r;
  r.side = Side::Buy;
  r.price = px;
  r.qty = q;
  return r;
}

RiskInputs healthy(Price mid = 1'000'000) {
  RiskInputs in;
  in.mid = mid;
  return in;
}

}  // namespace

TEST(Risk, PassesOrdinaryOrder) {
  RiskEngine r({});
  EXPECT_EQ(r.check_new(buy(1'000'000, 100), healthy(), 1), RiskCheck::Ok);
}

TEST(Risk, SizeNotionalCollar) {
  RiskLimits l;
  l.max_order_qty = 1000;
  l.max_order_notional = 50'000 * kPriceScale;
  l.price_collar = 0.02;
  RiskEngine r(l);
  EXPECT_EQ(r.check_new(buy(1'000'000, 1001), healthy(), 1), RiskCheck::MaxOrderQty);
  EXPECT_EQ(r.check_new(buy(1'000'000, 600), healthy(), 1), RiskCheck::MaxNotional);  // $60k
  EXPECT_EQ(r.check_new(buy(1'030'000, 10), healthy(), 1), RiskCheck::PriceCollar);   // +3%
  EXPECT_EQ(r.check_new(buy(1'010'000, 10), healthy(), 1), RiskCheck::Ok);
  EXPECT_EQ(r.check_new(buy(1'000'000, 10), healthy(kInvalidPrice), 1), RiskCheck::NoReferencePrice);
}

TEST(Risk, WorstCasePositionIncludesOpenOrders) {
  RiskLimits l;
  l.max_position = 1000;
  RiskEngine r(l);
  RiskInputs in = healthy();
  in.position = 500;
  in.open_buy_qty = 400;
  EXPECT_EQ(r.check_new(buy(1'000'000, 100), in, 1), RiskCheck::Ok);        // 500+400+100 = 1000
  EXPECT_EQ(r.check_new(buy(1'000'000, 101), in, 1), RiskCheck::MaxPosition);
  exec::OrderRequest sell = buy(1'000'000, 1500);
  sell.side = Side::Sell;
  EXPECT_EQ(r.check_new(sell, in, 1), RiskCheck::Ok);  // 500 - 1500 = -1000
  sell.qty = 1501;
  EXPECT_EQ(r.check_new(sell, in, 1), RiskCheck::MaxPosition);
}

TEST(Risk, HealthGatesComeFirst) {
  RiskEngine r({});
  RiskInputs in = healthy();
  in.market_data_ok = false;
  EXPECT_EQ(r.check_new(buy(1'000'000, 1), in, 1), RiskCheck::MarketDataStale);
  in.market_data_ok = true;
  in.venue_up = false;
  EXPECT_EQ(r.check_new(buy(1'000'000, 1), in, 1), RiskCheck::VenueDown);
}

TEST(Risk, TokenBucketRefillsOnSimulatedTime) {
  RiskLimits l;
  l.max_msgs_per_sec = 1000;  // one per ms
  l.burst = 5;
  l.throttle_trip = 1'000'000;
  RiskEngine r(l);
  TimestampNs t = 1'000'000'000;
  for (int i = 0; i < 5; ++i) EXPECT_EQ(r.check_new(buy(1'000'000, 1), healthy(), t), RiskCheck::Ok);
  EXPECT_EQ(r.check_new(buy(1'000'000, 1), healthy(), t), RiskCheck::RateLimit);
  t += 1'000'000;  // +1 ms -> one token
  EXPECT_EQ(r.check_new(buy(1'000'000, 1), healthy(), t), RiskCheck::Ok);
  EXPECT_EQ(r.check_new(buy(1'000'000, 1), healthy(), t), RiskCheck::RateLimit);
}

TEST(Risk, SustainedThrottlingTripsKillSwitch) {
  RiskLimits l;
  l.burst = 1;
  l.throttle_trip = 10;
  RiskEngine r(l);
  for (int i = 0; i < 11; ++i) r.check_new(buy(1'000'000, 1), healthy(), 5);
  EXPECT_TRUE(r.killed());
  EXPECT_EQ(r.check_new(buy(1'000'000, 1), healthy(), 10'000'000'000ull), RiskCheck::KillSwitch);
  r.reset_kill_switch();
  EXPECT_FALSE(r.killed());
}

TEST(Risk, MaxLossTrips) {
  RiskLimits l;
  l.max_loss = 1000 * kPriceScale;
  RiskEngine r(l);
  r.on_pnl(-999 * kPriceScale);
  EXPECT_FALSE(r.killed());
  r.on_pnl(-1001 * kPriceScale);
  EXPECT_TRUE(r.killed());
}

TEST(Risk, ReplaceOnlyCountsIncrease) {
  RiskLimits l;
  l.max_position = 100;
  RiskEngine r(l);
  RiskInputs in = healthy();
  in.position = 0;
  in.open_buy_qty = 100;  // the order being replaced
  EXPECT_EQ(r.check_replace(buy(1'000'000, 100), 100, in, 1), RiskCheck::Ok);
  EXPECT_EQ(r.check_replace(buy(1'000'000, 101), 100, in, 1), RiskCheck::MaxPosition);
}
