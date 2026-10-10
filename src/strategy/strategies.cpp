#include <algorithm>
#include <stdexcept>

#include "pulse/strategy/strategy.hpp"

namespace pulse::strategy {
namespace {

std::vector<bool> make_mask(const StrategyParams& p, std::size_t n) {
  std::vector<bool> m(n, p.instruments.empty());
  for (auto i : p.instruments)
    if (i < n) m[i] = true;
  return m;
}

// ---------------------------------------------------------------------------
// Passive market maker: joins the touch on both sides with one order per
// side, skews both quotes away from its inventory, and stops quoting a side
// once inventory reaches the limit. Requotes via cancel/replace, throttled
// per instrument. Not a profitable strategy -- a realistic *load*: it keeps
// orders resting, gets filled through the queue model and has to manage
// in-flight replaces, partial fills and cancel races.
// ---------------------------------------------------------------------------
class MarketMaker final : public Strategy {
 public:
  MarketMaker(const StrategyParams& p, std::size_t n) : p_(p), active_(make_mask(p, n)), s_(n) {}
  const char* name() const noexcept override { return "market-maker"; }

  void on_book(const BookUpdate& u, Context& ctx) override {
    if (u.has(BookUpdate::kFlagStatusOnly) || !active_[u.instrument] || !u.two_sided()) return;
    if (!ctx.trading_enabled()) return;
    State& s = s_[u.instrument];
    if (ctx.now() < s.last_quote + p_.min_requote_ns) return;
    const auto& spec = ctx.spec(u.instrument);
    const std::int64_t inv = ctx.position(u.instrument);
    // Skew: long inventory -> lower both quotes (sell more eagerly, buy less).
    const Price skew = -(inv * p_.skew_ticks / std::max<std::int64_t>(1, p_.max_inventory)) * spec.tick_size;
    Price bid = std::min(u.best_bid() + skew, u.best_ask() - spec.tick_size);
    Price ask = std::max(u.best_ask() + skew, u.best_bid() + spec.tick_size);
    bid -= bid % spec.tick_size;
    ask -= ask % spec.tick_size;
    bool attempted = false;
    attempted |= work(ctx, u.instrument, Side::Buy, s.bid, bid, inv < p_.max_inventory);
    attempted |= work(ctx, u.instrument, Side::Sell, s.ask, ask, inv > -p_.max_inventory);
    // Throttle on *attempts*, not successes: if risk rejects us, retrying on
    // every tick would just hammer the rate limiter (and trip the kill switch).
    if (attempted) s.last_quote = ctx.now();
  }

  void on_execution(const exec::ExecReport&, const exec::ClientOrder& o, Context&) override {
    if (!exec::is_terminal(o.state)) return;
    State& s = s_[o.instrument];
    if (s.bid == o.cl_id) s.bid = 0;
    if (s.ask == o.cl_id) s.ask = 0;
  }

  void on_halt(Context&) override {
    for (auto& s : s_) s = State{};
  }

 private:
  struct State {
    exec::ClOrdId bid = 0;
    exec::ClOrdId ask = 0;
    TimestampNs last_quote = 0;
  };

  bool work(Context& ctx, InstrumentId inst, Side side, exec::ClOrdId& id, Price px, bool allowed) {
    const exec::ClientOrder* o = id ? ctx.order(id) : nullptr;
    if (o && exec::is_terminal(o->state)) {
      id = 0;
      o = nullptr;
    }
    // Returns true if a request was attempted (whether or not risk passed it).
    if (!allowed) {
      if (o && o->state != exec::OrderState::PendingCancel) return ctx.cancel(id), true;
      return false;
    }
    if (!o) {
      id = ctx.submit(inst, side, px, p_.quote_size);
      return true;
    }
    // Only touch acknowledged, idle orders; one request in flight at a time.
    if (o->state != exec::OrderState::New && o->state != exec::OrderState::PartiallyFilled) return false;
    if (o->price == px) return false;
    ctx.replace(id, px, o->cum + p_.quote_size);
    return true;
  }

  StrategyParams p_;
  std::vector<bool> active_;
  std::vector<State> s_;
};

// ---------------------------------------------------------------------------
// Liquidity taker: when top-of-book size is heavily one-sided, cross the
// spread with an IOC in the direction of the imbalance; flatten inventory
// beyond the limit. Exercises marketable orders, IOC cancels and partial
// fills against displayed depth.
// ---------------------------------------------------------------------------
class ImbalanceTaker final : public Strategy {
 public:
  ImbalanceTaker(const StrategyParams& p, std::size_t n) : p_(p), active_(make_mask(p, n)), next_ok_(n, 0) {}
  const char* name() const noexcept override { return "imbalance"; }

  void on_book(const BookUpdate& u, Context& ctx) override {
    if (u.has(BookUpdate::kFlagStatusOnly) || !active_[u.instrument] || !u.two_sided()) return;
    if (!ctx.trading_enabled() || ctx.now() < next_ok_[u.instrument]) return;
    const double b = u.bid_qty[0], a = u.ask_qty[0];
    const double imb = (b - a) / (b + a);
    const std::int64_t pos = ctx.position(u.instrument);
    bool attempted = true;
    if (pos > p_.max_inventory) {
      ctx.submit(u.instrument, Side::Sell, u.best_bid(), p_.quote_size, exec::TimeInForce::IOC);
    } else if (pos < -p_.max_inventory) {
      ctx.submit(u.instrument, Side::Buy, u.best_ask(), p_.quote_size, exec::TimeInForce::IOC);
    } else if (imb > p_.imbalance_threshold) {
      ctx.submit(u.instrument, Side::Buy, u.best_ask(), p_.quote_size, exec::TimeInForce::IOC);
    } else if (imb < -p_.imbalance_threshold) {
      ctx.submit(u.instrument, Side::Sell, u.best_bid(), p_.quote_size, exec::TimeInForce::IOC);
    } else {
      attempted = false;
    }
    // Cool down after any attempt, accepted or rejected by risk.
    if (attempted) next_ok_[u.instrument] = ctx.now() + p_.cooldown_ns;
  }

 private:
  StrategyParams p_;
  std::vector<bool> active_;
  std::vector<TimestampNs> next_ok_;
};

// ---------------------------------------------------------------------------
// Failure drill: a "buggy" strategy stuck in a loop, firing several orders on
// every market update and never cancelling. Pre-trade risk must contain it:
// open-order and position limits, then the message-rate throttle, which trips
// the kill switch.
// ---------------------------------------------------------------------------
class Runaway final : public Strategy {
 public:
  explicit Runaway(const StrategyParams& p) : p_(p) {}
  const char* name() const noexcept override { return "runaway"; }
  void on_book(const BookUpdate& u, Context& ctx) override {
    if (u.has(BookUpdate::kFlagStatusOnly) || !u.two_sided()) return;
    // Note: deliberately ignores ctx.trading_enabled().
    for (std::uint32_t i = 0; i < p_.runaway_orders_per_update; ++i) {
      const Side side = (flip_++ & 1) ? Side::Buy : Side::Sell;
      const Price px = side == Side::Buy ? u.best_bid() : u.best_ask();
      ctx.submit(u.instrument, side, px, p_.quote_size, (i & 1) ? exec::TimeInForce::IOC : exec::TimeInForce::Day);
    }
  }

 private:
  StrategyParams p_;
  std::uint64_t flip_ = 0;
};

// Failure drill: throws after N updates. The core must contain the exception,
// disable this strategy, and cancel its working orders.
class Crash final : public Strategy {
 public:
  Crash(const StrategyParams& p, std::size_t n) : p_(p), mm_(p, n) {}
  const char* name() const noexcept override { return "crash"; }
  void on_book(const BookUpdate& u, Context& ctx) override {
    if (++updates_ >= p_.crash_after_updates) throw std::runtime_error("strategy bug: invariant violated");
    mm_.on_book(u, ctx);
  }
  void on_execution(const exec::ExecReport& r, const exec::ClientOrder& o, Context& ctx) override {
    mm_.on_execution(r, o, ctx);
  }

 private:
  StrategyParams p_;
  MarketMaker mm_;
  std::uint64_t updates_ = 0;
};

class None final : public Strategy {
 public:
  const char* name() const noexcept override { return "none"; }
  void on_book(const BookUpdate&, Context&) override {}
};

}  // namespace

std::unique_ptr<Strategy> make_strategy(const std::string& name, const StrategyParams& p, std::size_t n) {
  if (name == "none") return std::make_unique<None>();
  if (name == "market-maker" || name == "mm") return std::make_unique<MarketMaker>(p, n);
  if (name == "imbalance") return std::make_unique<ImbalanceTaker>(p, n);
  if (name == "runaway") return std::make_unique<Runaway>(p);
  if (name == "crash") return std::make_unique<Crash>(p, n);
  throw std::invalid_argument("unknown strategy: " + name);
}

std::vector<std::string> strategy_names() { return {"none", "market-maker", "imbalance", "runaway", "crash"}; }

}  // namespace pulse::strategy
