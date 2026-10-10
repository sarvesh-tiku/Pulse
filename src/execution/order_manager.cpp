#include "pulse/execution/order_manager.hpp"

#include <algorithm>

namespace pulse::exec {

const char* to_string(OrderState s) noexcept {
  switch (s) {
    case OrderState::PendingNew: return "PENDING_NEW";
    case OrderState::New: return "NEW";
    case OrderState::PartiallyFilled: return "PARTIALLY_FILLED";
    case OrderState::PendingCancel: return "PENDING_CANCEL";
    case OrderState::PendingReplace: return "PENDING_REPLACE";
    case OrderState::Filled: return "FILLED";
    case OrderState::Cancelled: return "CANCELLED";
    case OrderState::Rejected: return "REJECTED";
    case OrderState::Expired: return "EXPIRED";
  }
  return "?";
}

std::uint64_t OrderManager::contribution(const ClientOrder& o) noexcept {
  if (is_terminal(o.state)) return 0;
  std::uint64_t q = o.leaves();
  // A replace in flight may be accepted with the new size: count the larger.
  if (o.state == OrderState::PendingReplace && o.pending_qty > o.cum) {
    q = std::max<std::uint64_t>(q, o.pending_qty - o.cum);
  }
  return q;
}

OrderManager::Exposure& OrderManager::exposure(InstrumentId inst) {
  if (inst >= exposure_.size()) exposure_.resize(inst + 1);
  return exposure_[inst];
}

void OrderManager::unaccount(const ClientOrder& o) {
  auto& e = exposure(o.instrument);
  const auto c = contribution(o);
  (o.side == Side::Buy ? e.buy : e.sell) -= c;
  if (!is_terminal(o.state)) {
    --e.live;
    --live_total_;
  }
}

void OrderManager::account(const ClientOrder& o) {
  auto& e = exposure(o.instrument);
  const auto c = contribution(o);
  (o.side == Side::Buy ? e.buy : e.sell) += c;
  if (!is_terminal(o.state)) {
    ++e.live;
    ++live_total_;
  }
}

ClOrdId OrderManager::create(InstrumentId inst, Side side, Price px, Qty qty, TimeInForce tif,
                             StrategyId strategy, TimestampNs now) {
  ClientOrder o;
  o.cl_id = next_id_++;
  o.instrument = inst;
  o.side = side;
  o.price = px;
  o.qty = qty;
  o.tif = tif;
  o.strategy = strategy;
  o.sent_ts = o.last_ts = now;
  account(o);
  orders_.emplace(o.cl_id, o);
  ++stats_.orders;
  return o.cl_id;
}

bool OrderManager::begin_cancel(ClOrdId id, TimestampNs now) {
  auto it = orders_.find(id);
  if (it == orders_.end()) return false;
  ClientOrder& o = it->second;
  if (is_terminal(o.state) || o.state == OrderState::PendingCancel) return false;
  unaccount(o);
  if (o.state != OrderState::PendingReplace) o.state_before_pending = o.state;
  o.state = OrderState::PendingCancel;
  o.last_ts = now;
  account(o);
  return true;
}

bool OrderManager::begin_replace(ClOrdId id, Price px, Qty qty, TimestampNs now) {
  auto it = orders_.find(id);
  if (it == orders_.end()) return false;
  ClientOrder& o = it->second;
  if (is_terminal(o.state) || o.state == OrderState::PendingCancel || o.state == OrderState::PendingReplace ||
      o.state == OrderState::PendingNew) {
    return false;
  }
  unaccount(o);
  o.state_before_pending = o.state;
  o.state = OrderState::PendingReplace;
  o.pending_price = px;
  o.pending_qty = qty;
  o.last_ts = now;
  account(o);
  return true;
}

OrderManager::Update OrderManager::on_report(const ExecReport& r) {
  ++stats_.reports;
  Update u;
  if (r.type == ExecType::SessionDown || r.type == ExecType::SessionUp) return u;
  if (!seen_exec_ids_.insert(r.exec_id).second) {
    ++stats_.duplicate_reports;
    return u;
  }
  auto it = orders_.find(r.cl_id);
  if (it == orders_.end()) {
    ++stats_.unknown_order_reports;
    return u;
  }
  ClientOrder& o = it->second;
  u.order = &o;
  const bool was_terminal = is_terminal(o.state);
  unaccount(o);
  o.last_ts = std::max(o.last_ts, r.deliver_ts);

  auto settle_working = [&] {
    // Back to a working state after a pending request resolves.
    o.state = o.cum == 0 ? OrderState::New : (o.leaves() == 0 ? OrderState::Filled : OrderState::PartiallyFilled);
  };

  switch (r.type) {
    case ExecType::Accepted:
      o.exch_id = r.exch_id;
      o.acked = true;
      if (o.state == OrderState::PendingNew) settle_working();
      break;
    case ExecType::Rejected:
      if (!was_terminal) o.state = OrderState::Rejected;
      break;
    case ExecType::PartialFill:
    case ExecType::Fill:
      if (!o.acked && o.state == OrderState::PendingNew) ++stats_.fill_before_ack;
      if (was_terminal) ++stats_.late_fills;
      o.cum += r.last_qty;
      if (o.cum > o.qty) ++stats_.overfills;
      u.fill_qty = r.last_qty;
      u.fill_px = r.price;
      if (!was_terminal) {
        if (o.leaves() == 0) {
          o.state = OrderState::Filled;
        } else if (o.state == OrderState::PendingNew || o.state == OrderState::New ||
                   o.state == OrderState::PartiallyFilled) {
          o.state = OrderState::PartiallyFilled;
        }  // PendingCancel / PendingReplace stay pending
      }
      break;
    case ExecType::Cancelled:
      if (!was_terminal) o.state = OrderState::Cancelled;
      break;
    case ExecType::Replaced:
      if (!was_terminal) {
        o.price = r.price;
        o.qty = r.cum_qty + r.leaves_qty;
        // Fills may have been applied locally ahead of this report.
        o.qty = std::max(o.qty, o.cum);
        settle_working();
      }
      break;
    case ExecType::CancelRejected:
      ++stats_.cancel_rejects;
      if (o.state == OrderState::PendingCancel || o.state == OrderState::PendingReplace) settle_working();
      break;
    case ExecType::SessionDown:
    case ExecType::SessionUp: break;
  }
  account(o);
  u.became_terminal = !was_terminal && is_terminal(o.state);
  return u;
}

std::vector<ClOrdId> OrderManager::expire_stale(TimestampNs now, TimestampNs timeout) {
  std::vector<ClOrdId> out;
  for (auto& [id, o] : orders_) {
    // Never acknowledged by the venue (in any pending state: a cancel sent
    // for an order that never arrived is just as unanswerable) -> presumed
    // lost. Acknowledged orders are resolved by the venue's own reports,
    // including cancel-on-disconnect after a reconnect.
    if (o.acked || is_terminal(o.state)) continue;
    if (now < o.last_ts + timeout) continue;
    unaccount(o);
    o.state = OrderState::Expired;
    account(o);
    ++stats_.expired;
    out.push_back(id);
  }
  std::sort(out.begin(), out.end());  // unordered_map order is not deterministic across libs
  return out;
}

const ClientOrder* OrderManager::get(ClOrdId id) const {
  auto it = orders_.find(id);
  return it == orders_.end() ? nullptr : &it->second;
}

std::uint64_t OrderManager::open_qty(InstrumentId inst, Side side) const {
  if (inst >= exposure_.size()) return 0;
  return side == Side::Buy ? exposure_[inst].buy : exposure_[inst].sell;
}

std::size_t OrderManager::live_count(InstrumentId inst) const {
  return inst < exposure_.size() ? exposure_[inst].live : 0;
}

std::vector<ClOrdId> OrderManager::live_orders(InstrumentId inst) const {
  std::vector<ClOrdId> v;
  for (const auto& [id, o] : orders_)
    if (o.instrument == inst && !is_terminal(o.state)) v.push_back(id);
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<ClOrdId> OrderManager::live_orders() const {
  std::vector<ClOrdId> v;
  for (const auto& [id, o] : orders_)
    if (!is_terminal(o.state)) v.push_back(id);
  std::sort(v.begin(), v.end());
  return v;
}

}  // namespace pulse::exec
