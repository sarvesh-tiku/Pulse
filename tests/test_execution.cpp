#include <gtest/gtest.h>

#include <vector>

#include "pulse/execution/exchange_simulator.hpp"
#include "pulse/execution/order_manager.hpp"

using namespace pulse;
using namespace pulse::exec;

namespace {

const InstrumentUniverse& uni() {
  static const InstrumentUniverse u = InstrumentUniverse::synthetic(1);  // ALFA, ref 100.00
  return u;
}
constexpr Price T = 100;
constexpr Price P = 1'000'000;

BookUpdate book(TimestampNs ts, Price bid, std::uint32_t bq, Price ask, std::uint32_t aq) {
  BookUpdate u{};
  u.exchange_ts = ts;
  u.instrument = 0;
  u.bid_levels = u.ask_levels = 2;
  u.bid_px = {bid, bid - T};
  u.bid_qty = {bq, 1000};
  u.ask_px = {ask, ask + T};
  u.ask_qty = {aq, 1000};
  return u;
}

BookUpdate with_trade(BookUpdate u, Side resting, Price px, Qty q) {
  u.flags |= BookUpdate::kFlagTrade;
  u.trade_resting_side = resting;
  u.trade_px = px;
  u.trade_qty = q;
  return u;
}

OrderRequest new_order(ClOrdId id, Side s, Price px, Qty q, TimeInForce tif = TimeInForce::Day) {
  OrderRequest r;
  r.type = RequestType::New;
  r.cl_id = id;
  r.side = s;
  r.price = px;
  r.qty = q;
  r.tif = tif;
  return r;
}

ExchangeConfig quiet() {
  ExchangeConfig c;
  c.latency_in_ns = c.latency_out_ns = 1000;
  c.jitter_ns = 0;
  return c;
}

struct Reports {
  std::vector<ExecReport> v;
  ExchangeSimulator::ReportSink sink() {
    return [this](const ExecReport& r) { v.push_back(r); };
  }
  std::vector<ExecType> types() const {
    std::vector<ExecType> t;
    for (const auto& r : v) t.push_back(r.type);
    return t;
  }
};

}  // namespace

TEST(Exchange, LatencyDelaysArrivalAndReports) {
  ExchangeSimulator x(uni(), quiet());
  Reports r;
  x.on_market(book(0, P - T, 500, P + T, 500));
  x.submit(new_order(1, Side::Buy, P - 2 * T, 100), 0);
  x.advance_to(1500, r.sink());
  EXPECT_TRUE(r.v.empty());  // arrived at 1000, report due at 2000
  x.advance_to(2000, r.sink());
  ASSERT_EQ(r.v.size(), 1u);
  EXPECT_EQ(r.v[0].type, ExecType::Accepted);
  EXPECT_EQ(r.v[0].venue_ts, 1000u);
  EXPECT_EQ(r.v[0].deliver_ts, 2000u);
}

TEST(Exchange, MarketableOrderWalksDisplayedDepth) {
  ExchangeSimulator x(uni(), quiet());
  Reports r;
  x.on_market(book(0, P - T, 500, P + T, 300));
  x.submit(new_order(1, Side::Buy, P + 2 * T, 1500, TimeInForce::IOC), 0);
  x.drain(r.sink());
  ASSERT_EQ(r.types(), (std::vector<ExecType>{ExecType::Accepted, ExecType::PartialFill, ExecType::PartialFill,
                                             ExecType::Cancelled}));
  EXPECT_EQ(r.v[1].price, P + T);
  EXPECT_EQ(r.v[1].last_qty, 300u);
  EXPECT_EQ(r.v[2].price, P + 2 * T);
  EXPECT_EQ(r.v[2].last_qty, 1000u);  // all displayed at the second level
  EXPECT_EQ(r.v[3].cum_qty, 1300u);   // IOC remainder (200) cancelled
}

TEST(Exchange, SameLiquidityCannotBeTakenTwiceWithinOneUpdate) {
  ExchangeSimulator x(uni(), quiet());
  Reports r;
  x.on_market(book(0, P - T, 500, P + T, 300));
  x.submit(new_order(1, Side::Buy, P + T, 300, TimeInForce::IOC), 0);
  x.submit(new_order(2, Side::Buy, P + T, 300, TimeInForce::IOC), 0);
  x.drain(r.sink());
  std::uint64_t filled = 0;
  for (const auto& e : r.v) filled += e.last_qty;
  EXPECT_EQ(filled, 300u);
}

TEST(Exchange, RestingOrderWaitsForQueueAhead) {
  ExchangeSimulator x(uni(), quiet());
  Reports r;
  x.on_market(book(0, P, 400, P + T, 500));
  x.submit(new_order(1, Side::Buy, P, 100), 0);  // joins behind 400
  x.advance_to(5000, r.sink());
  x.on_market(with_trade(book(6000, P, 100, P + T, 500), Side::Buy, P, 300));  // 300 traded: 100 ahead
  x.advance_to(10'000, r.sink());
  EXPECT_EQ(r.types(), (std::vector<ExecType>{ExecType::Accepted}));
  x.on_market(with_trade(book(11'000, P, 50, P + T, 500), Side::Buy, P, 150));  // 100 ahead + 50 for us
  x.drain(r.sink());
  ASSERT_EQ(r.v.size(), 2u);
  EXPECT_EQ(r.v[1].type, ExecType::PartialFill);
  EXPECT_EQ(r.v[1].last_qty, 50u);
  EXPECT_EQ(r.v[1].price, P);
}

TEST(Exchange, TradeThroughOurPriceFillsUs) {
  ExchangeSimulator x(uni(), quiet());
  Reports r;
  x.on_market(book(0, P, 400, P + T, 500));
  x.submit(new_order(1, Side::Buy, P, 100), 0);
  x.advance_to(5000, r.sink());
  // Market prints below our bid on the buy side: price priority says we'd be hit.
  x.on_market(with_trade(book(6000, P - T, 100, P + T, 500), Side::Buy, P - T, 200));
  x.drain(r.sink());
  ASSERT_EQ(r.v.back().type, ExecType::Fill);
  EXPECT_EQ(r.v.back().price, P);  // passive fill at our price
}

TEST(Exchange, CancelAndTooLateToCancel) {
  ExchangeSimulator x(uni(), quiet());
  Reports r;
  x.on_market(book(0, P - T, 500, P + T, 300));
  x.submit(new_order(1, Side::Buy, P - 2 * T, 100), 0);
  OrderRequest c;
  c.type = RequestType::Cancel;
  c.orig_cl_id = 1;
  x.submit(c, 10);
  c.orig_cl_id = 42;
  x.submit(c, 20);
  x.drain(r.sink());
  EXPECT_EQ(r.types(), (std::vector<ExecType>{ExecType::Accepted, ExecType::Cancelled, ExecType::CancelRejected}));
  EXPECT_EQ(x.resting_orders(), 0u);
}

TEST(Exchange, DisconnectCancelsRestingAndReplaysAfterReconnect) {
  auto cfg = quiet();
  cfg.disconnects = {{10'000, 50'000}};
  ExchangeSimulator x(uni(), cfg);
  Reports r;
  x.on_market(book(0, P - T, 500, P + T, 300));
  x.submit(new_order(1, Side::Buy, P - 2 * T, 100), 0);
  x.submit(new_order(2, Side::Buy, P - 2 * T, 100), 20'000);  // sent while down: lost
  x.drain(r.sink());
  EXPECT_EQ(r.types(), (std::vector<ExecType>{ExecType::Accepted, ExecType::SessionDown, ExecType::SessionUp,
                                             ExecType::Cancelled}));
  EXPECT_EQ(r.v[3].cl_id, 1u);
  EXPECT_EQ(x.stats().dropped_while_down, 1u);
}

TEST(Exchange, ReorderedReportsCanOvertake) {
  auto cfg = quiet();
  cfg.jitter_ns = 50'000;
  cfg.reorder_reports = true;
  cfg.seed = 5;
  ExchangeSimulator x(uni(), cfg);
  Reports r;
  x.on_market(book(0, P - T, 500, P + T, 100'000));
  for (ClOrdId i = 1; i <= 200; ++i) x.submit(new_order(i, Side::Buy, P + T, 10, TimeInForce::IOC), i);
  x.drain(r.sink());
  bool overtaken = false;
  for (std::size_t i = 1; i < r.v.size(); ++i) overtaken |= r.v[i].exec_id < r.v[i - 1].exec_id;
  EXPECT_TRUE(overtaken);
}

// ---------------------------------------------------------------------------

TEST(Oms, HappyPathLifecycle) {
  OrderManager m;
  const auto id = m.create(0, Side::Buy, P, 100, TimeInForce::Day, 0, 0);
  EXPECT_EQ(m.get(id)->state, OrderState::PendingNew);
  EXPECT_EQ(m.open_qty(0, Side::Buy), 100u);
  ExecReport r{};
  r.cl_id = id;
  r.exec_id = 1;
  r.type = ExecType::Accepted;
  m.on_report(r);
  EXPECT_EQ(m.get(id)->state, OrderState::New);
  r.exec_id = 2;
  r.type = ExecType::PartialFill;
  r.last_qty = 40;
  r.price = P;
  auto u = m.on_report(r);
  EXPECT_EQ(u.fill_qty, 40u);
  EXPECT_EQ(m.get(id)->state, OrderState::PartiallyFilled);
  EXPECT_EQ(m.open_qty(0, Side::Buy), 60u);
  r.exec_id = 3;
  r.type = ExecType::Fill;
  r.last_qty = 60;
  u = m.on_report(r);
  EXPECT_TRUE(u.became_terminal);
  EXPECT_EQ(m.get(id)->state, OrderState::Filled);
  EXPECT_EQ(m.open_qty(0, Side::Buy), 0u);
  EXPECT_EQ(m.live_count(), 0u);
}

TEST(Oms, FillBeforeAckAndDuplicateReports) {
  OrderManager m;
  const auto id = m.create(0, Side::Sell, P, 100, TimeInForce::Day, 0, 0);
  ExecReport f{};
  f.cl_id = id;
  f.exec_id = 7;
  f.type = ExecType::PartialFill;
  f.last_qty = 30;
  EXPECT_EQ(m.on_report(f).fill_qty, 30u);
  EXPECT_EQ(m.on_report(f).fill_qty, 0u);  // duplicate exec id: applied once
  EXPECT_EQ(m.stats().fill_before_ack, 1u);
  EXPECT_EQ(m.stats().duplicate_reports, 1u);
  ExecReport a{};
  a.cl_id = id;
  a.exec_id = 6;
  a.type = ExecType::Accepted;
  m.on_report(a);  // late ack must not reset the fill state
  EXPECT_EQ(m.get(id)->state, OrderState::PartiallyFilled);
  EXPECT_EQ(m.get(id)->cum, 30u);
}

TEST(Oms, LateFillAfterCancelStillCounts) {
  OrderManager m;
  const auto id = m.create(0, Side::Buy, P, 100, TimeInForce::Day, 0, 0);
  ExecReport r{};
  r.cl_id = id;
  r.exec_id = 1;
  r.type = ExecType::Accepted;
  m.on_report(r);
  ASSERT_TRUE(m.begin_cancel(id, 5));
  r.exec_id = 3;
  r.type = ExecType::Cancelled;
  m.on_report(r);
  r.exec_id = 2;  // generated before the cancel, delivered after it
  r.type = ExecType::PartialFill;
  r.last_qty = 25;
  const auto u = m.on_report(r);
  EXPECT_EQ(u.fill_qty, 25u);  // positions must reflect it
  EXPECT_EQ(m.get(id)->state, OrderState::Cancelled);
  EXPECT_EQ(m.stats().late_fills, 1u);
  EXPECT_EQ(m.open_qty(0, Side::Buy), 0u);
}

TEST(Oms, CancelRejectRestoresWorkingState) {
  OrderManager m;
  const auto id = m.create(0, Side::Buy, P, 100, TimeInForce::Day, 0, 0);
  ExecReport r{};
  r.cl_id = id;
  r.exec_id = 1;
  r.type = ExecType::Accepted;
  m.on_report(r);
  ASSERT_TRUE(m.begin_replace(id, P + T, 300, 1));
  EXPECT_EQ(m.open_qty(0, Side::Buy), 300u);  // worst case of old/new size
  r.exec_id = 2;
  r.type = ExecType::CancelRejected;
  m.on_report(r);
  EXPECT_EQ(m.get(id)->state, OrderState::New);
  EXPECT_EQ(m.open_qty(0, Side::Buy), 100u);
}

TEST(Oms, UnansweredOrdersExpire) {
  OrderManager m;
  const auto id = m.create(0, Side::Buy, P, 100, TimeInForce::Day, 0, 1000);
  EXPECT_TRUE(m.expire_stale(1500, 1000).empty());
  const auto e = m.expire_stale(2000, 1000);
  ASSERT_EQ(e.size(), 1u);
  EXPECT_EQ(e[0], id);
  EXPECT_EQ(m.get(id)->state, OrderState::Expired);
  EXPECT_EQ(m.open_qty(0, Side::Buy), 0u);
}

// A new order in flight when the session drops never reaches the venue. The
// disconnect halt then sends a cancel for it, which is dropped too. The order
// must still resolve (Expired) rather than sit in PendingCancel forever.
TEST(Oms, UnackedOrderStuckInPendingCancelExpires) {
  auto cfg = quiet();
  cfg.disconnects = {{10'000, 50'000}};
  ExchangeSimulator x(uni(), cfg);
  OrderManager m;
  x.on_market(book(0, P - T, 500, P + T, 300));
  const auto id = m.create(0, Side::Buy, P - 2 * T, 100, TimeInForce::Day, 0, 9'500);
  x.submit(new_order(id, Side::Buy, P - 2 * T, 100), 9'500);  // arrives at 10'500: venue is down
  ASSERT_TRUE(m.begin_cancel(id, 10'000));                     // halt on SessionDown
  OrderRequest c;
  c.type = RequestType::Cancel;
  c.orig_cl_id = id;
  x.submit(c, 10'000);
  x.drain([&](const ExecReport& r) { m.on_report(r); });
  EXPECT_EQ(x.stats().dropped_while_down, 2u);
  EXPECT_EQ(m.get(id)->state, OrderState::PendingCancel);  // nobody will ever answer
  const auto expired = m.expire_stale(1'000'000, 500'000);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(m.get(id)->state, OrderState::Expired);
  EXPECT_EQ(m.live_count(), 0u);
}
