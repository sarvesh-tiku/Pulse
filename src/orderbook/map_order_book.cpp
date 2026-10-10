#include "pulse/orderbook/map_order_book.hpp"

namespace pulse::book {

const char* to_string(BookResult r) noexcept {
  switch (r) {
    case BookResult::Ok: return "ok";
    case BookResult::DuplicateOrderId: return "duplicate_order_id";
    case BookResult::UnknownOrder: return "unknown_order";
    case BookResult::BadPrice: return "bad_price";
    case BookResult::BadQty: return "bad_qty";
  }
  return "?";
}

MapOrderBook::PriceLevel& MapOrderBook::level_for(Side side, Price price) {
  return side == Side::Buy ? bids_[price] : asks_[price];
}

BookResult MapOrderBook::add(OrderId id, Side side, Price price, Qty qty) {
  if (qty == 0) return BookResult::BadQty;
  if (!spec_.in_band(price) || !spec_.on_tick(price)) return BookResult::BadPrice;
  if (id == 0 || index_.contains(id)) return BookResult::DuplicateOrderId;
  PriceLevel& lvl = level_for(side, price);
  lvl.fifo.push_back({id, qty});
  lvl.qty += qty;
  index_.emplace(id, Loc{side, price, std::prev(lvl.fifo.end())});
  return BookResult::Ok;
}

void MapOrderBook::erase_at(const Loc& loc) {
  auto erase_in = [&](auto& side_map) {
    auto lvl = side_map.find(loc.price);
    lvl->second.qty -= loc.it->qty;
    lvl->second.fifo.erase(loc.it);
    if (lvl->second.fifo.empty()) side_map.erase(lvl);
  };
  if (loc.side == Side::Buy) {
    erase_in(bids_);
  } else {
    erase_in(asks_);
  }
}

BookResult MapOrderBook::cancel(OrderId id) {
  auto it = index_.find(id);
  if (it == index_.end()) return BookResult::UnknownOrder;
  erase_at(it->second);
  index_.erase(it);
  return BookResult::Ok;
}

BookResult MapOrderBook::modify(OrderId id, Price price, Qty qty) {
  auto it = index_.find(id);
  if (it == index_.end()) return BookResult::UnknownOrder;
  if (qty == 0) return BookResult::BadQty;
  if (!spec_.in_band(price) || !spec_.on_tick(price)) return BookResult::BadPrice;
  Loc& loc = it->second;
  if (price == loc.price && qty <= loc.it->qty) {
    PriceLevel& lvl = level_for(loc.side, price);
    lvl.qty -= loc.it->qty - qty;
    loc.it->qty = qty;
    return BookResult::Ok;
  }
  const Side side = loc.side;
  erase_at(loc);
  PriceLevel& lvl = level_for(side, price);
  lvl.fifo.push_back({id, qty});
  lvl.qty += qty;
  loc = Loc{side, price, std::prev(lvl.fifo.end())};
  return BookResult::Ok;
}

BookResult MapOrderBook::execute(OrderId id, Qty qty) {
  auto it = index_.find(id);
  if (it == index_.end()) return BookResult::UnknownOrder;
  Loc& loc = it->second;
  if (qty == 0 || qty > loc.it->qty) return BookResult::BadQty;
  if (qty == loc.it->qty) {
    erase_at(loc);
    index_.erase(it);
  } else {
    loc.it->qty -= qty;
    level_for(loc.side, loc.price).qty -= qty;
  }
  return BookResult::Ok;
}

template <class Map>
Level MapOrderBook::level_of(const Map& m) noexcept {
  if (m.empty()) return {};
  const auto& [px, lvl] = *m.begin();
  return {px, lvl.qty, static_cast<std::uint32_t>(lvl.fifo.size())};
}

Level MapOrderBook::best_bid() const noexcept { return level_of(bids_); }
Level MapOrderBook::best_ask() const noexcept { return level_of(asks_); }

std::size_t MapOrderBook::depth(Side side, std::span<Level> out) const noexcept {
  std::size_t n = 0;
  auto fill = [&](const auto& m) {
    for (auto it = m.begin(); it != m.end() && n < out.size(); ++it, ++n) {
      out[n] = {it->first, it->second.qty, static_cast<std::uint32_t>(it->second.fifo.size())};
    }
  };
  if (side == Side::Buy) {
    fill(bids_);
  } else {
    fill(asks_);
  }
  return n;
}

bool MapOrderBook::find(OrderId id, OrderView& out) const noexcept {
  auto it = index_.find(id);
  if (it == index_.end()) return false;
  out = {id, it->second.side, it->second.price, it->second.it->qty};
  return true;
}

std::vector<OrderView> MapOrderBook::orders_at(Side side, Price price) const {
  std::vector<OrderView> v;
  auto collect = [&](const auto& m) {
    auto it = m.find(price);
    if (it == m.end()) return;
    for (const auto& o : it->second.fifo) v.push_back({o.id, side, price, o.qty});
  };
  if (side == Side::Buy) {
    collect(bids_);
  } else {
    collect(asks_);
  }
  return v;
}

void MapOrderBook::clear() {
  bids_.clear();
  asks_.clear();
  index_.clear();
}

}  // namespace pulse::book
