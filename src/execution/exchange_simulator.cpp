#include "pulse/execution/exchange_simulator.hpp"

#include <algorithm>
#include <limits>

#include "pulse/core/random.hpp"

namespace pulse::exec {

const char* to_string(ExecType t) noexcept {
  switch (t) {
    case ExecType::Accepted: return "ACCEPTED";
    case ExecType::Rejected: return "REJECTED";
    case ExecType::PartialFill: return "PARTIAL_FILL";
    case ExecType::Fill: return "FILL";
    case ExecType::Cancelled: return "CANCELLED";
    case ExecType::Replaced: return "REPLACED";
    case ExecType::CancelRejected: return "CANCEL_REJECTED";
    case ExecType::SessionDown: return "SESSION_DOWN";
    case ExecType::SessionUp: return "SESSION_UP";
  }
  return "?";
}

const char* to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None: return "none";
    case RejectReason::InvalidPrice: return "invalid_price";
    case RejectReason::InvalidQty: return "invalid_qty";
    case RejectReason::UnknownInstrument: return "unknown_instrument";
    case RejectReason::UnknownOrder: return "unknown_order";
    case RejectReason::VenueBusy: return "venue_busy";
    case RejectReason::TooLateToCancel: return "too_late_to_cancel";
  }
  return "?";
}

ExchangeSimulator::ExchangeSimulator(const InstrumentUniverse& universe, const ExchangeConfig& cfg)
    : universe_(universe), cfg_(cfg), rng_(cfg.seed), views_(universe.size()) {
  std::sort(cfg_.disconnects.begin(), cfg_.disconnects.end(),
            [](const DisconnectWindow& a, const DisconnectWindow& b) { return a.start < b.start; });
}

TimestampNs ExchangeSimulator::jitter() {
  if (cfg_.jitter_ns == 0) return 0;
  return rng_() % (cfg_.jitter_ns + 1);
}

bool ExchangeSimulator::connected_at(TimestampNs t) const noexcept {
  for (const auto& w : cfg_.disconnects)
    if (t >= w.start && t < w.end) return false;
  return true;
}

std::size_t ExchangeSimulator::resting_orders() const noexcept {
  std::size_t n = 0;
  for (const auto& v : views_) n += v.resting.size();
  return n;
}

void ExchangeSimulator::submit(const OrderRequest& req, TimestampNs now) {
  TimestampNs at = now + cfg_.latency_in_ns + jitter();
  at = std::max(at, last_arrival_);  // one ordered connection inbound
  last_arrival_ = at;
  inbound_.push({at, arrival_seq_++, req});
}

void ExchangeSimulator::report(ExecReport r, TimestampNs now) {
  r.exec_id = next_exec_id_++;
  r.venue_ts = now;
  TimestampNs d = now + cfg_.latency_out_ns + jitter();
  if (!cfg_.reorder_reports) d = std::max(d, last_delivery_);
  last_delivery_ = std::max(last_delivery_, d);
  r.deliver_ts = d;
  outbound_.push({r});
}

void ExchangeSimulator::advance_to(TimestampNs t, const ReportSink& sink) {
  constexpr auto kNever = std::numeric_limits<TimestampNs>::max();
  while (true) {
    const TimestampNs ta = inbound_.empty() ? kNever : inbound_.top().at;
    const TimestampNs td = outbound_.empty() ? kNever : outbound_.top().r.deliver_ts;
    TimestampNs tw = kNever;
    if (next_window_ < cfg_.disconnects.size()) {
      tw = down_ ? cfg_.disconnects[next_window_].end : cfg_.disconnects[next_window_].start;
    }
    const TimestampNs m = std::min({ta, td, tw});
    if (m > t || m == kNever) break;

    if (tw == m) {  // connection state changes first on ties
      if (!down_) {
        on_disconnect(tw);
      } else {
        on_reconnect(tw);
        ++next_window_;
      }
      continue;
    }
    if (ta == m && ta <= td) {
      const Arrival a = inbound_.top();
      inbound_.pop();
      process_arrival(a.req, a.at);
      continue;
    }
    const Outbound o = outbound_.top();
    outbound_.pop();
    if (down_ && o.r.type != ExecType::SessionDown) {
      deferred_.push_back(o.r);  // resent after reconnect (session recovery)
      ++stats_.reports_deferred;
      continue;
    }
    sink(o.r);
  }
}

void ExchangeSimulator::drain(const ReportSink& sink) {
  advance_to(std::numeric_limits<TimestampNs>::max() - 1, sink);
}

void ExchangeSimulator::on_disconnect(TimestampNs at) {
  down_ = true;
  ExecReport down{};
  down.type = ExecType::SessionDown;
  down.exec_id = next_exec_id_++;
  down.venue_ts = down.deliver_ts = at;  // the client notices immediately (socket closes)
  outbound_.push({down});
  if (!cfg_.cancel_on_disconnect) return;
  for (std::size_t i = 0; i < views_.size(); ++i) {
    for (const Resting& o : views_[i].resting) {
      ExecReport r{};
      r.type = ExecType::Cancelled;
      r.instrument = static_cast<InstrumentId>(i);
      r.side = o.side;
      r.cl_id = o.cl_id;
      r.exch_id = o.exch_id;
      r.price = o.price;
      r.cum_qty = o.cum;
      r.venue_ts = at;
      cancelled_while_down_.push_back(r);
      ++stats_.cancelled;
    }
    views_[i].resting.clear();
  }
}

void ExchangeSimulator::on_reconnect(TimestampNs at) {
  down_ = false;
  ExecReport up{};
  up.type = ExecType::SessionUp;
  up.exec_id = next_exec_id_++;
  up.venue_ts = up.deliver_ts = at;
  outbound_.push({up});
  // Replay what the client missed, after SessionUp: first reports that were
  // in flight (original exec ids, original order), then the cancels issued
  // by cancel-on-disconnect.
  for (ExecReport r : deferred_) {
    r.deliver_ts = at + 1;
    outbound_.push({r});
  }
  deferred_.clear();
  for (ExecReport r : cancelled_while_down_) {
    r.exec_id = next_exec_id_++;
    r.deliver_ts = at + 1;
    outbound_.push({r});
  }
  cancelled_while_down_.clear();
  last_delivery_ = std::max(last_delivery_, at + 1);
}

ExchangeSimulator::Resting* ExchangeSimulator::find(InstrumentView& v, ClOrdId cl_id) {
  for (auto& o : v.resting)
    if (o.cl_id == cl_id) return &o;
  return nullptr;
}

void ExchangeSimulator::process_arrival(const OrderRequest& req, TimestampNs now) {
  ++stats_.requests;
  if (down_) {
    ++stats_.dropped_while_down;  // never reached the venue
    return;
  }
  ExecReport r{};
  r.instrument = req.instrument;
  r.side = req.side;
  r.cl_id = req.type == RequestType::New ? req.cl_id : req.orig_cl_id;

  if (!universe_.contains(req.instrument)) {
    r.type = req.type == RequestType::New ? ExecType::Rejected : ExecType::CancelRejected;
    r.reason = RejectReason::UnknownInstrument;
    ++stats_.rejected;
    report(r, now);
    return;
  }
  const InstrumentSpec& spec = universe_[req.instrument];
  InstrumentView& v = views_[req.instrument];

  switch (req.type) {
    case RequestType::New: {
      RejectReason why = RejectReason::None;
      if (req.qty == 0) why = RejectReason::InvalidQty;
      else if (!spec.on_tick(req.price) || !spec.in_band(req.price)) why = RejectReason::InvalidPrice;
      else if (bernoulli(rng_, cfg_.reject_probability))
        why = RejectReason::VenueBusy;
      if (why != RejectReason::None) {
        r.type = ExecType::Rejected;
        r.reason = why;
        r.price = req.price;
        ++stats_.rejected;
        report(r, now);
        return;
      }
      Resting o{req.cl_id, next_exch_id_++, req.side, req.tif, req.price, req.qty, 0, 0};
      r.type = ExecType::Accepted;
      r.exch_id = o.exch_id;
      r.price = o.price;
      r.leaves_qty = o.leaves;
      ++stats_.accepted;
      report(r, now);
      match(v, o, /*at_own_price=*/false, now);
      if (o.leaves == 0) return;
      if (o.tif == TimeInForce::IOC) {
        ExecReport c = r;
        c.type = ExecType::Cancelled;
        c.leaves_qty = 0;
        c.cum_qty = o.cum;
        ++stats_.cancelled;
        report(c, now);
        return;
      }
      // Join the back of the queue at our price.
      o.queue_ahead = 0;
      if (v.have_book) {
        const auto& b = v.book;
        const auto& px = o.side == Side::Buy ? b.bid_px : b.ask_px;
        const auto& qty = o.side == Side::Buy ? b.bid_qty : b.ask_qty;
        const auto n = o.side == Side::Buy ? b.bid_levels : b.ask_levels;
        for (std::size_t i = 0; i < n; ++i)
          if (px[i] == o.price) o.queue_ahead = qty[i];
      }
      v.resting.push_back(o);
      return;
    }
    case RequestType::Cancel: {
      auto it = std::find_if(v.resting.begin(), v.resting.end(),
                             [&](const Resting& o) { return o.cl_id == req.orig_cl_id; });
      if (it == v.resting.end()) {
        r.type = ExecType::CancelRejected;
        r.reason = RejectReason::TooLateToCancel;
        ++stats_.cancel_rejects;
        report(r, now);
        return;
      }
      r.type = ExecType::Cancelled;
      r.side = it->side;
      r.exch_id = it->exch_id;
      r.price = it->price;
      r.cum_qty = it->cum;
      v.resting.erase(it);
      ++stats_.cancelled;
      report(r, now);
      return;
    }
    case RequestType::Replace: {
      Resting* o = find(v, req.orig_cl_id);
      if (o == nullptr) {
        r.type = ExecType::CancelRejected;
        r.reason = RejectReason::TooLateToCancel;
        ++stats_.cancel_rejects;
        report(r, now);
        return;
      }
      if (!spec.on_tick(req.price) || !spec.in_band(req.price) || req.qty == 0) {
        r.type = ExecType::CancelRejected;
        r.reason = req.qty == 0 ? RejectReason::InvalidQty : RejectReason::InvalidPrice;
        ++stats_.cancel_rejects;
        report(r, now);
        return;
      }
      r.side = o->side;
      r.exch_id = o->exch_id;
      if (req.qty <= o->cum) {  // replace down to/below filled qty == cancel
        r.type = ExecType::Cancelled;
        r.price = o->price;
        r.cum_qty = o->cum;
        v.resting.erase(v.resting.begin() + (o - v.resting.data()));
        ++stats_.cancelled;
        report(r, now);
        return;
      }
      const Qty new_leaves = req.qty - o->cum;
      const bool keeps_priority = req.price == o->price && new_leaves <= o->leaves;
      if (!keeps_priority) {
        o->queue_ahead = 0;
        const auto& b = v.book;
        const auto& px = o->side == Side::Buy ? b.bid_px : b.ask_px;
        const auto& qty = o->side == Side::Buy ? b.bid_qty : b.ask_qty;
        const auto n = o->side == Side::Buy ? b.bid_levels : b.ask_levels;
        for (std::size_t i = 0; i < n; ++i)
          if (px[i] == req.price) o->queue_ahead = qty[i];
      }
      o->price = req.price;
      o->leaves = new_leaves;
      r.type = ExecType::Replaced;
      r.price = o->price;
      r.leaves_qty = o->leaves;
      r.cum_qty = o->cum;
      ++stats_.replaced;
      report(r, now);
      match(v, *o, false, now);
      if (o->leaves == 0) v.resting.erase(v.resting.begin() + (o - v.resting.data()));
      return;
    }
  }
}

void ExchangeSimulator::fill(InstrumentView& v, Resting& o, Price px, Qty qty, TimestampNs now) {
  (void)v;
  o.leaves -= qty;
  o.cum += qty;
  ExecReport r{};
  r.type = o.leaves == 0 ? ExecType::Fill : ExecType::PartialFill;
  r.side = o.side;
  r.cl_id = o.cl_id;
  r.exch_id = o.exch_id;
  r.price = px;
  r.last_qty = qty;
  r.leaves_qty = o.leaves;
  r.cum_qty = o.cum;
  // instrument filled by caller context
  r.instrument = static_cast<InstrumentId>(&v - views_.data());
  ++stats_.fills;
  stats_.filled_qty += qty;
  report(r, now);
}

void ExchangeSimulator::match(InstrumentView& v, Resting& o, bool at_own_price, TimestampNs now) {
  if (!v.have_book || o.leaves == 0) return;
  const BookUpdate& b = v.book;
  const Side opp = opposite(o.side);
  const auto& px = o.side == Side::Buy ? b.ask_px : b.bid_px;
  const auto& qty = o.side == Side::Buy ? b.ask_qty : b.bid_qty;
  const auto n = o.side == Side::Buy ? b.ask_levels : b.bid_levels;
  for (std::size_t i = 0; i < n && o.leaves > 0; ++i) {
    const bool crosses = o.side == Side::Buy ? px[i] <= o.price : px[i] >= o.price;
    if (!crosses) break;
    auto avail = static_cast<std::uint64_t>(static_cast<double>(qty[i]) * cfg_.fill_ratio);
    Consumed* c = nullptr;
    for (auto& e : v.consumed)
      if (e.side == opp && e.price == px[i]) c = &e;
    if (c) avail = avail > c->qty ? avail - c->qty : 0;
    if (avail == 0) continue;
    const Qty q = static_cast<Qty>(std::min<std::uint64_t>(avail, o.leaves));
    if (c) {
      c->qty += q;
    } else {
      v.consumed.push_back({opp, px[i], q});
    }
    fill(v, o, at_own_price ? o.price : px[i], q, now);
  }
}

void ExchangeSimulator::on_resting_market(InstrumentView& v, Resting& o, const BookUpdate& u) {
  const TimestampNs now = u.exchange_ts;
  // 1) Prints on our side of the book.
  if (u.has(BookUpdate::kFlagTrade) && u.trade_resting_side == o.side && o.leaves > 0) {
    const bool through = o.side == Side::Buy ? o.price > u.trade_px : o.price < u.trade_px;
    if (through) {
      // Someone traded at a worse price than ours while we were resting:
      // price priority says we would have been hit first.
      fill(v, o, o.price, std::min(o.leaves, u.trade_qty), now);
    } else if (u.trade_px == o.price) {
      if (o.queue_ahead >= u.trade_qty) {
        o.queue_ahead -= u.trade_qty;
      } else {
        const auto excess = static_cast<Qty>(u.trade_qty - o.queue_ahead);
        o.queue_ahead = 0;
        fill(v, o, o.price, std::min(o.leaves, excess), now);
      }
    }
  }
  // 2) The opposite side moved through our price.
  if (o.leaves > 0) match(v, o, /*at_own_price=*/true, now);
  // 3) Cancels ahead of us: our queue position can't exceed the level size.
  if (o.leaves > 0 && o.queue_ahead > 0) {
    const auto& px = o.side == Side::Buy ? u.bid_px : u.ask_px;
    const auto& qty = o.side == Side::Buy ? u.bid_qty : u.ask_qty;
    const auto n = o.side == Side::Buy ? u.bid_levels : u.ask_levels;
    bool visible = false;
    std::uint64_t level_qty = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (px[i] == o.price) level_qty = qty[i];
    }
    if (n > 0) {
      const Price worst = px[n - 1];
      visible = o.side == Side::Buy ? o.price >= worst : o.price <= worst;
    }
    if (visible || n < kBookDepth) o.queue_ahead = std::min(o.queue_ahead, level_qty);
  }
}

void ExchangeSimulator::on_market(const BookUpdate& u) {
  if (u.has(BookUpdate::kFlagStatusOnly) || !universe_.contains(u.instrument)) return;
  InstrumentView& v = views_[u.instrument];
  v.book = u;
  v.have_book = true;
  v.consumed.clear();
  if (down_ || v.resting.empty()) return;
  for (auto& o : v.resting) on_resting_market(v, o, u);
  std::erase_if(v.resting, [](const Resting& o) { return o.leaves == 0; });
}

}  // namespace pulse::exec
