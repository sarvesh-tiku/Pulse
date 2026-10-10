#include "pulse/risk/risk_engine.hpp"

#include <algorithm>
#include <cstdlib>

namespace pulse::risk {

const char* to_string(RiskCheck c) noexcept {
  switch (c) {
    case RiskCheck::Ok: return "ok";
    case RiskCheck::KillSwitch: return "kill_switch";
    case RiskCheck::MarketDataStale: return "market_data_stale";
    case RiskCheck::VenueDown: return "venue_down";
    case RiskCheck::NoReferencePrice: return "no_reference_price";
    case RiskCheck::MaxOrderQty: return "max_order_qty";
    case RiskCheck::MaxNotional: return "max_notional";
    case RiskCheck::PriceCollar: return "price_collar";
    case RiskCheck::MaxPosition: return "max_position";
    case RiskCheck::MaxOpenOrders: return "max_open_orders";
    case RiskCheck::RateLimit: return "rate_limit";
    case RiskCheck::kCount: break;
  }
  return "?";
}

RiskCheck RiskEngine::record(RiskCheck c) {
  ++counts_[static_cast<std::size_t>(c)];
  return c;
}

bool RiskEngine::take_token(TimestampNs now) {
  if (!have_time_) {
    have_time_ = true;
    last_refill_ = now;
  }
  if (now > last_refill_) {
    const double elapsed_s = static_cast<double>(now - last_refill_) * 1e-9;
    tokens_ = std::min(limits_.burst, tokens_ + elapsed_s * limits_.max_msgs_per_sec);
    last_refill_ = now;
  }
  if (tokens_ < 1.0) return false;
  tokens_ -= 1.0;
  return true;
}

RiskCheck RiskEngine::common(const exec::OrderRequest& req, std::uint64_t added_qty, const RiskInputs& in,
                             TimestampNs now) {
  if (killed_) return record(RiskCheck::KillSwitch);
  if (!in.market_data_ok) return record(RiskCheck::MarketDataStale);
  if (!in.venue_up) return record(RiskCheck::VenueDown);
  if (req.qty > limits_.max_order_qty) return record(RiskCheck::MaxOrderQty);
  if (req.price * static_cast<portfolio::Money>(req.qty) > limits_.max_order_notional) {
    return record(RiskCheck::MaxNotional);
  }
  if (in.mid == kInvalidPrice) return record(RiskCheck::NoReferencePrice);
  const double dev = std::abs(static_cast<double>(req.price - in.mid)) / static_cast<double>(in.mid);
  if (dev > limits_.price_collar) return record(RiskCheck::PriceCollar);
  // Worst case: every open order on this side fills, plus this one.
  const auto added = static_cast<std::int64_t>(added_qty);
  if (req.side == Side::Buy) {
    if (in.position + static_cast<std::int64_t>(in.open_buy_qty) + added > limits_.max_position) {
      return record(RiskCheck::MaxPosition);
    }
  } else {
    if (in.position - static_cast<std::int64_t>(in.open_sell_qty) - added < -limits_.max_position) {
      return record(RiskCheck::MaxPosition);
    }
  }
  if (!take_token(now)) {
    // Trip only on sustained throttling: count within a rolling 1 s window.
    if (now > throttle_window_start_ + 1'000'000'000) {
      throttle_window_start_ = now;
      throttled_ = 0;
    }
    if (++throttled_ >= limits_.throttle_trip) trip("order rate: throttle limit exceeded");
    return record(RiskCheck::RateLimit);
  }
  return record(RiskCheck::Ok);
}

RiskCheck RiskEngine::check_new(const exec::OrderRequest& req, const RiskInputs& in, TimestampNs now) {
  if (!killed_ && in.open_orders >= limits_.max_open_orders) return record(RiskCheck::MaxOpenOrders);
  return common(req, req.qty, in, now);
}

RiskCheck RiskEngine::check_replace(const exec::OrderRequest& req, Qty current_leaves, const RiskInputs& in,
                                    TimestampNs now) {
  // Only an increase in size adds exposure.
  const std::uint64_t added = req.qty > current_leaves ? req.qty - current_leaves : 0;
  return common(req, added, in, now);
}

void RiskEngine::on_cancel(TimestampNs now) { (void)take_token(now); }

void RiskEngine::on_pnl(portfolio::Money total_pnl) {
  if (!killed_ && total_pnl < -limits_.max_loss) trip("max loss breached");
}

void RiskEngine::trip(const std::string& reason) {
  if (killed_) return;
  killed_ = true;
  kill_reason_ = reason;
}

void RiskEngine::reset_kill_switch() {
  killed_ = false;
  kill_reason_.clear();
  throttled_ = 0;
}

}  // namespace pulse::risk
