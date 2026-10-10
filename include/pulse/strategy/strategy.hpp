#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pulse/core/instrument.hpp"
#include "pulse/execution/order_manager.hpp"
#include "pulse/orderbook/book_update.hpp"

namespace pulse::strategy {

// What a strategy may do. Implemented by the trading core; every order goes
// through pre-trade risk before it can reach the venue, so a strategy can
// never bypass limits.
class Context {
 public:
  virtual ~Context() = default;
  virtual TimestampNs now() const noexcept = 0;
  // Returns the client order id, or 0 if pre-trade risk rejected it.
  virtual exec::ClOrdId submit(InstrumentId inst, Side side, Price px, Qty qty,
                               exec::TimeInForce tif = exec::TimeInForce::Day) = 0;
  virtual bool cancel(exec::ClOrdId id) = 0;
  virtual bool replace(exec::ClOrdId id, Price px, Qty qty) = 0;
  virtual std::int64_t position(InstrumentId inst) const noexcept = 0;
  virtual const exec::ClientOrder* order(exec::ClOrdId id) const = 0;
  virtual const InstrumentSpec& spec(InstrumentId inst) const noexcept = 0;
  // False while the kill switch is engaged, market data is unhealthy or the
  // venue session is down. Strategies should not even try to send then.
  virtual bool trading_enabled() const noexcept = 0;
};

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual const char* name() const noexcept = 0;
  virtual void on_book(const BookUpdate& u, Context& ctx) = 0;
  virtual void on_execution(const exec::ExecReport& r, const exec::ClientOrder& o, Context& ctx) {
    (void)r, (void)o, (void)ctx;
  }
  // Trading was halted and the core cancelled every working order.
  virtual void on_halt(Context& ctx) { (void)ctx; }
};

struct StrategyParams {
  std::vector<InstrumentId> instruments;  // empty: all
  Qty quote_size = 100;
  std::int64_t max_inventory = 2'000;
  Price skew_ticks = 2;                    // inventory skew at max inventory
  TimestampNs min_requote_ns = 5'000'000;  // per instrument (<= 400 msgs/s each)
  double imbalance_threshold = 0.65;
  TimestampNs cooldown_ns = 5'000'000;
  std::uint32_t runaway_orders_per_update = 4;
  std::uint64_t crash_after_updates = 50'000;
};

// Names: "none", "market-maker", "imbalance", "runaway", "crash".
std::unique_ptr<Strategy> make_strategy(const std::string& name, const StrategyParams& params,
                                        std::size_t num_instruments);
std::vector<std::string> strategy_names();

}  // namespace pulse::strategy
