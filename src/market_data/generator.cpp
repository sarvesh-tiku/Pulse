#include "pulse/market_data/generator.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "pulse/core/random.hpp"

namespace pulse::md {
namespace {

struct Mix {
  double add, cancel, modify, aggressive;
};

Mix mix_for(WorkloadProfile p) {
  switch (p) {
    case WorkloadProfile::Balanced: return {0.42, 0.36, 0.12, 0.10};
    case WorkloadProfile::AddHeavy: return {0.60, 0.22, 0.10, 0.08};
    case WorkloadProfile::CancelHeavy: return {0.47, 0.46, 0.04, 0.03};
    case WorkloadProfile::TradeHeavy: return {0.45, 0.20, 0.10, 0.25};
    case WorkloadProfile::Burst: return {0.42, 0.36, 0.12, 0.10};
  }
  return {0.42, 0.36, 0.12, 0.10};
}

}  // namespace

const char* to_string(WorkloadProfile p) noexcept {
  switch (p) {
    case WorkloadProfile::Balanced: return "balanced";
    case WorkloadProfile::AddHeavy: return "add-heavy";
    case WorkloadProfile::CancelHeavy: return "cancel-heavy";
    case WorkloadProfile::TradeHeavy: return "trade-heavy";
    case WorkloadProfile::Burst: return "burst";
  }
  return "?";
}

bool parse_profile(const std::string& s, WorkloadProfile& out) noexcept {
  for (auto p : {WorkloadProfile::Balanced, WorkloadProfile::AddHeavy, WorkloadProfile::CancelHeavy,
                 WorkloadProfile::TradeHeavy, WorkloadProfile::Burst}) {
    if (s == to_string(p)) {
      out = p;
      return true;
    }
  }
  return false;
}

FeedGenerator::FeedGenerator(const GeneratorConfig& cfg, const InstrumentUniverse& universe)
    : cfg_(cfg), universe_(universe), rng_(cfg.seed), now_(cfg.start_ts) {
  if (universe_.size() == 0) throw std::invalid_argument("generator needs at least one instrument");
  inst_.resize(universe_.size());
  for (std::size_t i = 0; i < inst_.size(); ++i) {
    const auto& spec = universe_[static_cast<InstrumentId>(i)];
    inst_[i].fair = spec.reference_price;
    inst_[i].book = std::make_unique<book::FastOrderBook>(spec, 4096);
  }
}

bool FeedGenerator::next(MarketEvent& out) {
  if (produced_ >= cfg_.events) return false;
  if (produced_ + 1 == cfg_.events) {
    // Trailing heartbeat: lets a receiver detect loss of the final messages.
    out = MarketEvent{};
    out.type = EventType::Heartbeat;
    // Announce the sequence after the last event actually emitted. Events of
    // an unfinished sweep still in `pending_` were never sent: announcing
    // next_seq_ here would make receivers see a phantom gap.
    out.seq = pending_.empty() ? next_seq_ : pending_.front().seq;
    out.exchange_ts = now_;
    ++produced_;
    return true;
  }
  while (pending_.empty()) step();
  out = pending_.front();
  pending_.pop_front();
  ++produced_;
  return true;
}

void FeedGenerator::emit(MarketEvent ev) {
  ev.seq = next_seq_++;
  ev.exchange_ts = now_;
  pending_.push_back(ev);
}

Qty FeedGenerator::draw_qty() {
  // Mostly round lots, occasionally odd lots and large prints.
  const auto r = rng_() % 100;
  if (r < 8) return static_cast<Qty>(1 + rng_() % 99);
  if (r < 95) return static_cast<Qty>(100 * (1 + rng_() % 10));
  return static_cast<Qty>(100 * (10 + rng_() % 40));
}

std::uint64_t FeedGenerator::draw_gap() {
  double mean = cfg_.mean_gap_ns;
  if (cfg_.profile == WorkloadProfile::Burst) {
    if (regime_left_ == 0) {
      burst_ = !burst_;
      regime_left_ = burst_ ? 2'000 + rng_() % 20'000 : 5'000 + rng_() % 50'000;
    }
    --regime_left_;
    mean = burst_ ? mean / 20.0 : mean * 3.0;
  }
  return static_cast<std::uint64_t>(exponential(rng_, mean)) + 1;
}

void FeedGenerator::add_live(InstrumentState& s, OrderId id, Side side) {
  s.where.insert(id, static_cast<std::uint32_t>(s.live.size()));
  s.live.push_back({id, side});
}

void FeedGenerator::remove_live(InstrumentState& s, OrderId id) {
  const auto i = s.where.find(id);
  const Live last = s.live.back();
  s.live[i] = last;
  s.live.pop_back();
  s.where.erase(id);
  if (last.id != id) s.where.assign(last.id, i);
}

void FeedGenerator::step() {
  now_ += draw_gap();
  const auto inst = static_cast<InstrumentId>(rng_() % inst_.size());
  InstrumentState& s = inst_[inst];
  const auto& spec = universe_[inst];

  // Fair value random walk, kept inside the middle half of the price band so
  // the book never runs into the band edges.
  if (rng_() % 64 == 0) {
    const Price quarter = (spec.max_price - spec.min_price) / 4;
    const Price step = (rng_() & 1) ? spec.tick_size : -spec.tick_size;
    s.fair = std::clamp(s.fair + step, spec.min_price + quarter, spec.max_price - quarter);
    s.fair -= s.fair % spec.tick_size;
  }

  const Mix m = mix_for(cfg_.profile);
  const std::size_t target = 2 * cfg_.target_orders_per_side;
  double r = uniform01(rng_);
  if (s.live.size() < target / 2) r = 0.0;             // force add
  else if (s.live.size() > target * 3 / 2) r = m.add;  // force cancel

  if (r < m.add) {
    do_add(inst);
  } else if (r < m.add + m.cancel) {
    do_cancel(inst);
  } else if (r < m.add + m.cancel + m.modify) {
    do_modify(inst);
  } else {
    const Side aggressor = (rng_() & 1) ? Side::Buy : Side::Sell;
    const auto opp = aggressor == Side::Buy ? s.book->best_ask() : s.book->best_bid();
    if (opp.price == kInvalidPrice) {
      do_add(inst);
      return;
    }
    // Sweep up to two levels through the touch.
    const Price through = static_cast<Price>(rng_() % 3) * spec.tick_size;
    const Price limit = aggressor == Side::Buy ? opp.price + through : opp.price - through;
    do_aggressive(inst, aggressor, draw_qty(), std::clamp(limit, spec.min_price, spec.max_price));
  }
}

void FeedGenerator::do_add(InstrumentId inst) {
  InstrumentState& s = inst_[inst];
  const auto& spec = universe_[inst];
  const Side side = (rng_() & 1) ? Side::Buy : Side::Sell;
  const Price offset = static_cast<Price>(1 + geometric(rng_, 0.25)) * spec.tick_size;
  Price px = side == Side::Buy ? s.fair - offset : s.fair + offset;
  // Passive orders must not cross the opposite touch.
  if (side == Side::Buy) {
    const auto ask = s.book->best_ask();
    if (ask.price != kInvalidPrice) px = std::min(px, ask.price - spec.tick_size);
  } else {
    const auto bid = s.book->best_bid();
    if (bid.price != kInvalidPrice) px = std::max(px, bid.price + spec.tick_size);
  }
  px = std::clamp(px, spec.min_price, spec.max_price);
  const Qty qty = draw_qty();
  const OrderId id = next_order_id_++;
  if (s.book->add(id, side, px, qty) != book::BookResult::Ok) return;
  add_live(s, id, side);
  MarketEvent ev{};
  ev.type = EventType::AddOrder;
  ev.instrument = inst;
  ev.order_id = id;
  ev.side = side;
  ev.price = px;
  ev.qty = qty;
  emit(ev);
}

void FeedGenerator::do_cancel(InstrumentId inst) {
  InstrumentState& s = inst_[inst];
  if (s.live.empty()) return do_add(inst);
  const Live l = s.live[rng_() % s.live.size()];
  s.book->cancel(l.id);
  remove_live(s, l.id);
  MarketEvent ev{};
  ev.type = EventType::CancelOrder;
  ev.instrument = inst;
  ev.order_id = l.id;
  emit(ev);
}

void FeedGenerator::do_modify(InstrumentId inst) {
  InstrumentState& s = inst_[inst];
  const auto& spec = universe_[inst];
  if (s.live.empty()) return do_add(inst);
  const Live l = s.live[rng_() % s.live.size()];
  book::OrderView o;
  s.book->find(l.id, o);
  Price px = o.price;
  Qty qty = o.qty;
  if (rng_() & 1) {
    // Partial cancel: keeps queue priority.
    if (qty <= 1) return do_cancel(inst);
    qty = static_cast<Qty>(1 + rng_() % (qty - 1));
  } else {
    // Reprice by one or two ticks; loses priority.
    const Price d = static_cast<Price>(1 + rng_() % 2) * spec.tick_size;
    px = (rng_() & 1) ? px + d : px - d;
    if (l.side == Side::Buy) {
      const auto ask = s.book->best_ask();
      if (ask.price != kInvalidPrice) px = std::min(px, ask.price - spec.tick_size);
    } else {
      const auto bid = s.book->best_bid();
      if (bid.price != kInvalidPrice) px = std::max(px, bid.price + spec.tick_size);
    }
    px = std::clamp(px, spec.min_price, spec.max_price);
    qty = draw_qty();
  }
  if (s.book->modify(l.id, px, qty) != book::BookResult::Ok) return;
  MarketEvent ev{};
  ev.type = EventType::ModifyOrder;
  ev.instrument = inst;
  ev.order_id = l.id;
  ev.side = l.side;
  ev.price = px;
  ev.qty = qty;
  emit(ev);
}

void FeedGenerator::do_aggressive(InstrumentId inst, Side aggressor, Qty qty, Price limit) {
  InstrumentState& s = inst_[inst];
  const Side resting = opposite(aggressor);
  while (qty > 0) {
    book::OrderView head;
    if (!s.book->front(resting, head)) break;
    if (aggressor == Side::Buy ? head.price > limit : head.price < limit) break;
    const Qty fill = std::min(qty, head.qty);
    MarketEvent ev{};
    ev.type = EventType::Trade;
    ev.instrument = inst;
    ev.order_id = head.id;
    ev.side = resting;
    ev.price = head.price;
    ev.qty = fill;
    emit(ev);
    s.book->execute(head.id, fill);
    if (fill == head.qty) remove_live(s, head.id);
    qty -= fill;
  }
  // Sometimes the unfilled remainder rests at the limit (it cannot cross: the
  // sweep consumed everything at or through the limit).
  if (qty > 0 && rng_() % 3 == 0) {
    const OrderId id = next_order_id_++;
    if (s.book->add(id, aggressor, limit, qty) != book::BookResult::Ok) return;
    add_live(s, id, aggressor);
    MarketEvent ev{};
    ev.type = EventType::AddOrder;
    ev.instrument = inst;
    ev.order_id = id;
    ev.side = aggressor;
    ev.price = limit;
    ev.qty = qty;
    emit(ev);
  }
}

}  // namespace pulse::md
