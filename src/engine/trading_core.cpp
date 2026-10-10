#include "pulse/engine/trading_core.hpp"

#include <algorithm>
#include <exception>

#include "pulse/core/clock.hpp"

namespace pulse::engine {

TradingCore::TradingCore(const InstrumentUniverse& uni, const CoreConfig& cfg,
                         std::vector<std::unique_ptr<strategy::Strategy>> strategies)
    : uni_(uni),
      cfg_(cfg),
      strategies_(std::move(strategies)),
      strategy_enabled_(strategies_.size(), true),
      exchange_(uni, cfg.exchange),
      risk_(cfg.risk),
      portfolio_(uni.size()),
      latest_(uni.size()),
      have_book_(uni.size(), false) {
  sink_ = [this](const exec::ExecReport& r) { handle_report(r); };
}

bool TradingCore::trading_enabled() const noexcept {
  return !risk_.killed() && venue_up_ && !gap_open_ && !stale_ && !finished_;
}

risk::RiskInputs TradingCore::inputs(InstrumentId inst) const {
  risk::RiskInputs in;
  in.mid = have_book_[inst] ? latest_[inst].mid() : kInvalidPrice;
  in.position = portfolio_[inst].qty();
  in.open_buy_qty = oms_.open_qty(inst, Side::Buy);
  in.open_sell_qty = oms_.open_qty(inst, Side::Sell);
  in.open_orders = oms_.live_count(inst);
  in.market_data_ok = !gap_open_ && !stale_;
  in.venue_up = venue_up_;
  return in;
}

void TradingCore::incident(const std::string& what) {
  if (incidents_.size() < 1000) incidents_.push_back(what);
}

void TradingCore::on_book_update(const BookUpdate& u) {
  ++stats_.updates;
  now_ = std::max(now_, u.exchange_ts);
  // Venue events (arrivals, fills, reports) that happened before this market
  // event are processed first, in simulated-time order.
  current_ingress_ = 0;
  exchange_.advance_to(now_, sink_);

  if (u.has(BookUpdate::kFlagStatusOnly)) {
    digest_.mix(0x5354415455530000ull | u.flags);
    if (u.cause == EventType::FeedGap) {
      gap_open_ = true;
      const std::uint64_t missing = u.trade_qty;  // carried by the book stage
      if (cfg_.cancel_on_feed_gap && missing >= cfg_.gap_cancel_threshold) {
        incident("feed gap of " + std::to_string(missing) + " msgs from seq " + std::to_string(u.seq) +
                 ": pulling working orders");
        halt("feed gap");
      }
    } else if (u.cause == EventType::FeedRecovered) {
      gap_open_ = false;
    } else if (u.cause == EventType::FeedStale) {
      gap_open_ = false;
      if (!stale_) {
        stale_ = true;
        incident("feed STALE at seq " + std::to_string(u.seq) + ": trading halted for the session");
        halt("feed stale");
      }
    }
    return;
  }

  exchange_.on_market(u);
  latest_[u.instrument] = u;
  have_book_[u.instrument] = true;
  portfolio_.mark(u.instrument, u.mid());

  if (now_ >= next_expiry_check_) {
    for (auto id : oms_.expire_stale(now_, cfg_.order_timeout_ns)) {
      ++stats_.expired_orders;
      digest_.mix(0xE0000000ull ^ id);
      incident("order " + std::to_string(id) + " expired: no venue response");
    }
    next_expiry_check_ = now_ + 10'000'000;
  }
  if ((stats_.updates & 4095) == 0) risk_.on_pnl(portfolio_.total());

  if (risk_.killed() && !kill_handled_) {
    kill_handled_ = true;
    incident("KILL SWITCH: " + risk_.kill_reason());
    halt("kill switch");
  }

  current_ingress_ = cfg_.measure_latency ? u.ingress_ns : 0;
  for (std::size_t i = 0; i < strategies_.size(); ++i) {
    if (strategy_enabled_[i]) invoke(i, u);
  }
  current_ingress_ = 0;

  if (risk_.killed() && !kill_handled_) {
    kill_handled_ = true;
    incident("KILL SWITCH: " + risk_.kill_reason());
    halt("kill switch");
  }
}

void TradingCore::invoke(std::size_t idx, const BookUpdate& u) {
  current_strategy_ = static_cast<std::uint16_t>(idx);
  try {
    strategies_[idx]->on_book(u, *this);
  } catch (const std::exception& e) {
    // Contain the failure: disable the strategy and pull its orders. The
    // rest of the system keeps running.
    ++stats_.strategy_exceptions;
    strategy_enabled_[idx] = false;
    incident(std::string("strategy '") + strategies_[idx]->name() + "' crashed: " + e.what() +
             " -> disabled, orders cancelled");
    digest_.mix(0xC4A5000000ull + idx);
    for (auto id : oms_.live_orders()) {
      const auto* o = oms_.get(id);
      if (o && o->strategy == idx) cancel(id);
    }
  }
}

void TradingCore::halt(const char* why) {
  ++stats_.halts;
  (void)why;
  for (auto id : oms_.live_orders()) cancel(id);
  for (std::size_t i = 0; i < strategies_.size(); ++i) {
    if (strategy_enabled_[i]) strategies_[i]->on_halt(*this);
  }
}

exec::ClOrdId TradingCore::submit(InstrumentId inst, Side side, Price px, Qty qty, exec::TimeInForce tif) {
  if (!uni_.contains(inst)) return 0;
  exec::OrderRequest req;
  req.type = exec::RequestType::New;
  req.instrument = inst;
  req.side = side;
  req.price = px;
  req.qty = qty;
  req.tif = tif;
  req.strategy = current_strategy_;
  req.sent_ts = now_;
  const auto verdict = risk_.check_new(req, inputs(inst), now_);
  digest_.mix(0x71500000ull | static_cast<std::uint64_t>(verdict));
  if (verdict != risk::RiskCheck::Ok) {
    ++stats_.risk_rejects;
    return 0;
  }
  req.cl_id = oms_.create(inst, side, px, qty, tif, current_strategy_, now_);
  exchange_.submit(req, now_);
  ++stats_.orders;
  digest_.mix(req.cl_id);
  if (current_ingress_) tick_to_order_.record(now_ns() - current_ingress_);
  return req.cl_id;
}

bool TradingCore::cancel(exec::ClOrdId id) {
  const auto* o = oms_.get(id);
  if (!o || !oms_.begin_cancel(id, now_)) return false;
  risk_.on_cancel(now_);
  exec::OrderRequest req;
  req.type = exec::RequestType::Cancel;
  req.orig_cl_id = id;
  req.instrument = o->instrument;
  req.side = o->side;
  req.sent_ts = now_;
  exchange_.submit(req, now_);
  ++stats_.cancels;
  digest_.mix(0xCA000000ull ^ id);
  return true;
}

bool TradingCore::replace(exec::ClOrdId id, Price px, Qty qty) {
  const auto* o = oms_.get(id);
  if (!o || exec::is_terminal(o->state)) return false;
  exec::OrderRequest req;
  req.type = exec::RequestType::Replace;
  req.orig_cl_id = id;
  req.instrument = o->instrument;
  req.side = o->side;
  req.price = px;
  req.qty = qty;
  req.sent_ts = now_;
  const auto verdict = risk_.check_replace(req, o->leaves(), inputs(o->instrument), now_);
  digest_.mix(0x72500000ull | static_cast<std::uint64_t>(verdict));
  if (verdict != risk::RiskCheck::Ok) {
    ++stats_.risk_rejects;
    return false;
  }
  if (!oms_.begin_replace(id, px, qty, now_)) return false;
  exchange_.submit(req, now_);
  ++stats_.replaces;
  if (current_ingress_) tick_to_order_.record(now_ns() - current_ingress_);
  return true;
}

void TradingCore::handle_report(const exec::ExecReport& r) {
  now_ = std::max(now_, r.deliver_ts);
  ++stats_.reports;
  digest_.mix(r.exec_id);
  digest_.mix(static_cast<std::uint64_t>(r.type));
  digest_.mix(r.cl_id);
  digest_.mix(r.last_qty);
  digest_.mix_signed(r.price);

  if (r.type == exec::ExecType::SessionDown) {
    venue_up_ = false;
    incident("venue session DOWN");
    halt("venue down");
    return;
  }
  if (r.type == exec::ExecType::SessionUp) {
    venue_up_ = true;
    incident("venue session UP");
    return;
  }

  const auto upd = oms_.on_report(r);
  if (!upd.order) return;
  if (upd.fill_qty) {
    portfolio_.apply_fill(upd.order->instrument, upd.order->side, upd.fill_px, upd.fill_qty);
    ++stats_.fills;
    stats_.filled_qty += upd.fill_qty;
    risk_.on_pnl(portfolio_.total());
  }
  const auto sid = upd.order->strategy;
  if (sid < strategies_.size() && strategy_enabled_[sid]) {
    current_strategy_ = sid;
    try {
      strategies_[sid]->on_execution(r, *upd.order, *this);
    } catch (const std::exception& e) {
      ++stats_.strategy_exceptions;
      strategy_enabled_[sid] = false;
      incident(std::string("strategy '") + strategies_[sid]->name() + "' crashed in on_execution: " + e.what());
    }
  }
}

void TradingCore::finish() {
  if (finished_) return;
  current_ingress_ = 0;
  exchange_.advance_to(now_, sink_);
  for (auto id : oms_.live_orders()) cancel(id);
  finished_ = true;
  exchange_.drain(sink_);
  risk_.on_pnl(portfolio_.total());
  for (std::size_t i = 0; i < portfolio_.size(); ++i) {
    const auto& p = portfolio_[static_cast<InstrumentId>(i)];
    digest_.mix_signed(p.qty());
    digest_.mix_signed(p.realized());
    digest_.mix_signed(p.open_cost());
  }
  publish_snapshot();
}

void TradingCore::publish_snapshot() {
  CoreSnapshot s;
  s.updates = stats_.updates;
  s.orders = stats_.orders;
  s.fills = stats_.fills;
  s.risk_rejects = stats_.risk_rejects;
  s.realized = portfolio_.realized();
  s.unrealized = portfolio_.unrealized();
  s.live_orders = static_cast<std::uint32_t>(oms_.live_count());
  s.killed = risk_.killed();
  s.venue_up = venue_up_;
  s.md_ok = !gap_open_ && !stale_;
  s.md_stale = stale_;
  s.instruments = static_cast<std::uint32_t>(std::min(uni_.size(), CoreSnapshot::kMaxInstruments));
  for (std::size_t i = 0; i < s.instruments; ++i) {
    const auto inst = static_cast<InstrumentId>(i);
    s.bid[i] = have_book_[i] ? latest_[i].best_bid() : kInvalidPrice;
    s.ask[i] = have_book_[i] ? latest_[i].best_ask() : kInvalidPrice;
    s.position[i] = portfolio_[inst].qty();
    s.inst_pnl[i] = portfolio_[inst].realized() + portfolio_[inst].unrealized(portfolio_.mark_of(inst));
  }
  snapshot_.store(s);
}

}  // namespace pulse::engine
