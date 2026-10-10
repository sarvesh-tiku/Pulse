#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "pulse/core/types.hpp"
#include "pulse/execution/order.hpp"
#include "pulse/portfolio/position.hpp"

namespace pulse::risk {

struct RiskLimits {
  Qty max_order_qty = 5'000;
  portfolio::Money max_order_notional = 1'000'000 * kPriceScale;  // $1M
  std::int64_t max_position = 20'000;          // |shares| per instrument, worst case
  std::size_t max_open_orders = 64;            // per instrument
  double price_collar = 0.05;                  // |limit - mid| / mid
  // Token bucket over *simulated* time (all order messages incl. cancels).
  double max_msgs_per_sec = 10'000.0;
  double burst = 500.0;
  // Automatic kill switch triggers.
  std::uint32_t throttle_trip = 500;           // rate-limited messages within 1 s before tripping
  portfolio::Money max_loss = 50'000 * kPriceScale;  // total P&L below -max_loss
};

enum class RiskCheck : std::uint8_t {
  Ok = 0,
  KillSwitch,
  MarketDataStale,   // feed gap open or feed stale: book not trustworthy
  VenueDown,
  NoReferencePrice,  // one-sided or empty book: cannot apply the collar
  MaxOrderQty,
  MaxNotional,
  PriceCollar,
  MaxPosition,
  MaxOpenOrders,
  RateLimit,
  kCount,
};

const char* to_string(RiskCheck c) noexcept;

// Context the engine needs about the world, supplied by the trading core.
struct RiskInputs {
  Price mid = kInvalidPrice;
  std::int64_t position = 0;
  std::uint64_t open_buy_qty = 0;   // worst-case additional buys
  std::uint64_t open_sell_qty = 0;
  std::size_t open_orders = 0;
  bool market_data_ok = true;
  bool venue_up = true;
};

// Pre-trade risk. Runs synchronously on the trading thread between the
// strategy and the gateway: nothing reaches the venue without passing it.
// Checks are ordered cheapest / most-severe first. Cancels are always
// allowed (they only reduce risk) except that they still count toward the
// message-rate limit.
class RiskEngine {
 public:
  explicit RiskEngine(const RiskLimits& limits) : limits_(limits), tokens_(limits.burst) {}

  RiskCheck check_new(const exec::OrderRequest& req, const RiskInputs& in, TimestampNs now);
  RiskCheck check_replace(const exec::OrderRequest& req, Qty current_leaves, const RiskInputs& in,
                          TimestampNs now);
  // Cancels consume a rate-limit token but are never rejected for it.
  void on_cancel(TimestampNs now);

  // Post-trade: called with the current total P&L after fills / marks.
  void on_pnl(portfolio::Money total_pnl);

  void trip(const std::string& reason);
  void reset_kill_switch();
  bool killed() const noexcept { return killed_; }
  const std::string& kill_reason() const noexcept { return kill_reason_; }
  const std::array<std::uint64_t, static_cast<std::size_t>(RiskCheck::kCount)>& counts() const noexcept {
    return counts_;
  }
  const RiskLimits& limits() const noexcept { return limits_; }

 private:
  RiskCheck record(RiskCheck c);
  bool take_token(TimestampNs now);
  RiskCheck common(const exec::OrderRequest& req, std::uint64_t added_qty, const RiskInputs& in, TimestampNs now);

  RiskLimits limits_;
  double tokens_;
  TimestampNs last_refill_ = 0;
  bool have_time_ = false;
  bool killed_ = false;
  std::string kill_reason_;
  std::uint32_t throttled_ = 0;
  TimestampNs throttle_window_start_ = 0;
  std::array<std::uint64_t, static_cast<std::size_t>(RiskCheck::kCount)> counts_{};
};

}  // namespace pulse::risk
