#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pulse/execution/order.hpp"

namespace pulse::exec {

enum class OrderState : std::uint8_t {
  PendingNew,
  New,
  PartiallyFilled,
  PendingCancel,
  PendingReplace,
  Filled,     // terminal
  Cancelled,  // terminal
  Rejected,   // terminal
  Expired,    // terminal: no response within the timeout (request presumed lost)
};

const char* to_string(OrderState s) noexcept;
constexpr bool is_terminal(OrderState s) noexcept { return s >= OrderState::Filled; }

struct ClientOrder {
  ClOrdId cl_id = 0;
  ExchOrdId exch_id = 0;
  InstrumentId instrument = 0;
  Side side = Side::Buy;
  StrategyId strategy = 0;
  TimeInForce tif = TimeInForce::Day;
  OrderState state = OrderState::PendingNew;
  OrderState state_before_pending = OrderState::New;
  bool acked = false;
  Price price = 0;
  Qty qty = 0;
  Qty cum = 0;
  Price pending_price = 0;  // replace in flight
  Qty pending_qty = 0;
  TimestampNs sent_ts = 0;
  TimestampNs last_ts = 0;

  Qty leaves() const noexcept { return qty > cum ? qty - cum : 0; }
};

struct OmsStats {
  std::uint64_t orders = 0;
  std::uint64_t reports = 0;
  std::uint64_t unknown_order_reports = 0;
  std::uint64_t duplicate_reports = 0;
  std::uint64_t fill_before_ack = 0;    // out-of-order: fill overtook the ack
  std::uint64_t late_fills = 0;         // fill after cancel/terminal (cancel/fill race)
  std::uint64_t overfills = 0;
  std::uint64_t expired = 0;
  std::uint64_t cancel_rejects = 0;
};

// Order state machine on the client side.
//
// Principle: the venue is the source of truth for *fills*; our order state is
// a best-effort view. Fills are applied exactly once (deduplicated by
// exec_id) regardless of the order in which reports arrive or of the state
// the order is in — a fill that arrives after a cancel acknowledgement still
// happened, and positions are derived from fills, not from order states.
class OrderManager {
 public:
  struct Update {
    const ClientOrder* order = nullptr;  // null if the report matched nothing
    Qty fill_qty = 0;
    Price fill_px = 0;
    bool became_terminal = false;
  };

  ClOrdId create(InstrumentId inst, Side side, Price px, Qty qty, TimeInForce tif, StrategyId strategy,
                 TimestampNs now);
  // Marks a cancel / replace in flight. False if the order cannot be acted on.
  bool begin_cancel(ClOrdId id, TimestampNs now);
  bool begin_replace(ClOrdId id, Price px, Qty qty, TimestampNs now);

  Update on_report(const ExecReport& r);

  // Orders never acknowledged by the venue within `timeout` are presumed lost.
  std::vector<ClOrdId> expire_stale(TimestampNs now, TimestampNs timeout);

  const ClientOrder* get(ClOrdId id) const;
  // Worst-case quantity that could still execute, per side, per instrument.
  std::uint64_t open_qty(InstrumentId inst, Side side) const;
  std::size_t live_count(InstrumentId inst) const;
  std::size_t live_count() const noexcept { return live_total_; }
  std::vector<ClOrdId> live_orders(InstrumentId inst) const;
  std::vector<ClOrdId> live_orders() const;
  const OmsStats& stats() const noexcept { return stats_; }

 private:
  struct Exposure {
    std::uint64_t buy = 0;
    std::uint64_t sell = 0;
    std::size_t live = 0;
  };
  static std::uint64_t contribution(const ClientOrder& o) noexcept;
  void unaccount(const ClientOrder& o);
  void account(const ClientOrder& o);
  Exposure& exposure(InstrumentId inst);

  ClOrdId next_id_ = 1;
  std::unordered_map<ClOrdId, ClientOrder> orders_;
  std::unordered_set<std::uint64_t> seen_exec_ids_;
  std::vector<Exposure> exposure_;
  std::size_t live_total_ = 0;
  OmsStats stats_;
};

}  // namespace pulse::exec
