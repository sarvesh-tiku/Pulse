#pragma once

#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "pulse/orderbook/book_types.hpp"

namespace pulse::book {

// Baseline implementation: the "textbook" book.
//   price levels  std::map<Price, Level>   (red-black tree, O(log L))
//   FIFO queue    std::list<Order>         (O(1) erase via stored iterator)
//   order index   std::unordered_map       (O(1) average)
//
// Every add allocates a list node, a hash node and sometimes a tree node;
// every lookup chases pointers. It is correct and simple, which makes it the
// reference model for differential tests, and the "before" in benchmarks.
class MapOrderBook {
 public:
  explicit MapOrderBook(const InstrumentSpec& spec) : spec_(spec) {}

  BookResult add(OrderId id, Side side, Price price, Qty qty);
  BookResult cancel(OrderId id);
  BookResult modify(OrderId id, Price price, Qty qty);
  BookResult execute(OrderId id, Qty qty);

  Level best_bid() const noexcept;
  Level best_ask() const noexcept;
  std::size_t depth(Side side, std::span<Level> out) const noexcept;
  std::size_t order_count() const noexcept { return index_.size(); }
  bool find(OrderId id) const noexcept { return index_.contains(id); }
  bool find(OrderId id, OrderView& out) const noexcept;
  std::vector<OrderView> orders_at(Side side, Price price) const;
  const InstrumentSpec& spec() const noexcept { return spec_; }
  void clear();

 private:
  struct Order {
    OrderId id;
    Qty qty;
  };
  struct PriceLevel {
    std::uint64_t qty = 0;
    std::list<Order> fifo;
  };
  using Bids = std::map<Price, PriceLevel, std::greater<>>;
  using Asks = std::map<Price, PriceLevel, std::less<>>;
  struct Loc {
    Side side;
    Price price;
    std::list<Order>::iterator it;
  };

  template <class Map>
  static Level level_of(const Map& m) noexcept;
  PriceLevel& level_for(Side side, Price price);
  void erase_at(const Loc& loc);

  InstrumentSpec spec_;
  Bids bids_;
  Asks asks_;
  std::unordered_map<OrderId, Loc> index_;
};

static_assert(OrderBook<MapOrderBook>);

}  // namespace pulse::book
