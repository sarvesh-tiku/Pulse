#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pulse/concurrency/seqlock.hpp"
#include "pulse/core/instrument.hpp"
#include "pulse/engine/digest.hpp"
#include "pulse/execution/exchange_simulator.hpp"
#include "pulse/execution/order_manager.hpp"
#include "pulse/portfolio/position.hpp"
#include "pulse/risk/risk_engine.hpp"
#include "pulse/strategy/strategy.hpp"
#include "pulse/telemetry/histogram.hpp"

namespace pulse::engine {

struct CoreConfig {
  exec::ExchangeConfig exchange;
  risk::RiskLimits risk;
  TimestampNs order_timeout_ns = 500'000'000;  // PendingNew with no reply -> Expired
  // While any gap is open, new orders are blocked (risk: MarketDataStale).
  // Working orders are additionally pulled if the gap is at least this many
  // messages (a one-frame reorder heals in microseconds; a burst loss may
  // hide a sweep through our price). Stale always pulls everything.
  bool cancel_on_feed_gap = true;
  std::uint64_t gap_cancel_threshold = 64;
  bool measure_latency = true;                 // wall-clock stamps (off for pure digest runs)
};

struct CoreStats {
  std::uint64_t updates = 0;
  std::uint64_t orders = 0;
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t reports = 0;
  std::uint64_t fills = 0;
  std::uint64_t filled_qty = 0;
  std::uint64_t halts = 0;               // cancel-all events (gap, stale, kill, venue down)
  std::uint64_t strategy_exceptions = 0;
  std::uint64_t expired_orders = 0;
};

// Fixed-size snapshot published to the telemetry thread via a seqlock.
struct CoreSnapshot {
  static constexpr std::size_t kMaxInstruments = 16;
  std::uint64_t updates = 0;
  std::uint64_t orders = 0;
  std::uint64_t fills = 0;
  std::uint64_t risk_rejects = 0;
  std::int64_t realized = 0;
  std::int64_t unrealized = 0;
  std::uint32_t live_orders = 0;
  std::uint8_t killed = 0;
  std::uint8_t venue_up = 1;
  std::uint8_t md_ok = 1;
  std::uint8_t md_stale = 0;
  std::uint32_t instruments = 0;
  std::array<Price, kMaxInstruments> bid{};
  std::array<Price, kMaxInstruments> ask{};
  std::array<std::int64_t, kMaxInstruments> position{};
  std::array<std::int64_t, kMaxInstruments> inst_pnl{};
};

// The deterministic heart of the system: strategy, pre-trade risk, order
// management, venue simulation and positions, all on one thread, driven only
// by the ordered stream of BookUpdates and the simulated venue clock.
//
// Why single-threaded: every piece of state here is mutually dependent
// (risk needs positions and open orders; positions depend on fills; the
// strategy needs all of it). Splitting it across threads would need locks or
// message passing on the critical path and would make results depend on
// scheduling. Keeping it on one core (the LMAX "business logic processor"
// pattern) makes it fast *and* replayable; parallelism is used around it,
// for I/O, decoding and book building.
class TradingCore final : public strategy::Context {
 public:
  TradingCore(const InstrumentUniverse& uni, const CoreConfig& cfg,
              std::vector<std::unique_ptr<strategy::Strategy>> strategies);

  void on_book_update(const BookUpdate& u);
  // End of session: cancel working orders, let the venue drain, final marks.
  void finish();

  // strategy::Context
  TimestampNs now() const noexcept override { return now_; }
  exec::ClOrdId submit(InstrumentId inst, Side side, Price px, Qty qty, exec::TimeInForce tif) override;
  bool cancel(exec::ClOrdId id) override;
  bool replace(exec::ClOrdId id, Price px, Qty qty) override;
  std::int64_t position(InstrumentId inst) const noexcept override { return portfolio_[inst].qty(); }
  const exec::ClientOrder* order(exec::ClOrdId id) const override { return oms_.get(id); }
  const InstrumentSpec& spec(InstrumentId inst) const noexcept override { return uni_[inst]; }
  bool trading_enabled() const noexcept override;

  const CoreStats& stats() const noexcept { return stats_; }
  const exec::OrderManager& oms() const noexcept { return oms_; }
  const risk::RiskEngine& risk() const noexcept { return risk_; }
  const exec::ExchangeSimulator& exchange() const noexcept { return exchange_; }
  const portfolio::Portfolio& portfolio() const noexcept { return portfolio_; }
  const Digest& digest() const noexcept { return digest_; }
  const telemetry::LatencyHistogram& tick_to_order() const noexcept { return tick_to_order_; }
  const std::vector<std::string>& incidents() const noexcept { return incidents_; }
  const conc::SeqLock<CoreSnapshot>& snapshot() const noexcept { return snapshot_; }
  void publish_snapshot();

 private:
  void handle_report(const exec::ExecReport& r);
  void halt(const char* why);
  void incident(const std::string& what);
  risk::RiskInputs inputs(InstrumentId inst) const;
  void invoke(std::size_t idx, const BookUpdate& u);

  const InstrumentUniverse& uni_;
  CoreConfig cfg_;
  std::vector<std::unique_ptr<strategy::Strategy>> strategies_;
  std::vector<bool> strategy_enabled_;
  exec::ExchangeSimulator exchange_;
  exec::OrderManager oms_;
  risk::RiskEngine risk_;
  portfolio::Portfolio portfolio_;
  std::vector<BookUpdate> latest_;
  std::vector<bool> have_book_;
  exec::ExchangeSimulator::ReportSink sink_;

  TimestampNs now_ = 0;
  TimestampNs next_expiry_check_ = 0;
  std::uint16_t current_strategy_ = 0;
  std::uint64_t current_ingress_ = 0;
  bool venue_up_ = true;
  bool gap_open_ = false;
  bool stale_ = false;
  bool kill_handled_ = false;
  bool finished_ = false;

  CoreStats stats_;
  Digest digest_;
  telemetry::LatencyHistogram tick_to_order_;
  std::vector<std::string> incidents_;
  conc::SeqLock<CoreSnapshot> snapshot_;
};

}  // namespace pulse::engine
