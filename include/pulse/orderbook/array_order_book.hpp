#pragma once

#include <bit>
#include <cassert>
#include <unordered_map>
#include <vector>

#include "pulse/core/flat_id_map.hpp"
#include "pulse/orderbook/book_types.hpp"

namespace pulse::book {

// Order-id -> pool-index adaptors, so the effect of the hash map can be
// measured separately from the effect of the price-level layout.
struct FlatIndex {
  static constexpr std::uint32_t kNotFound = FlatIdMap::kNotFound;
  explicit FlatIndex(std::size_t expected) : map(expected) {}
  std::uint32_t find(OrderId id) const noexcept { return map.find(id); }
  bool insert(OrderId id, std::uint32_t v) { return map.insert(id, v); }
  void erase(OrderId id) noexcept { map.erase(id); }
  std::size_t size() const noexcept { return map.size(); }
  void clear() noexcept { map.clear(); }
  FlatIdMap map;
};

struct StdIndex {
  static constexpr std::uint32_t kNotFound = 0xFFFF'FFFFu;
  explicit StdIndex(std::size_t expected) { map.reserve(expected); }
  std::uint32_t find(OrderId id) const noexcept {
    auto it = map.find(id);
    return it == map.end() ? kNotFound : it->second;
  }
  bool insert(OrderId id, std::uint32_t v) { return map.emplace(id, v).second; }
  void erase(OrderId id) noexcept { map.erase(id); }
  std::size_t size() const noexcept { return map.size(); }
  void clear() noexcept { map.clear(); }
  std::unordered_map<OrderId, std::uint32_t> map;
};

// Optimized implementation: a price-indexed book.
//
//   price levels  flat array, one slot per tick inside the instrument band:
//                 slot = (price - min_price) / tick_size. O(1) access, no
//                 tree rebalancing, adjacent prices are adjacent in memory.
//   best price    cached slot + a bitmap of non-empty levels; when the best
//                 level empties, the next one is found with count-leading /
//                 trailing-zeros over 64-level words.
//   FIFO queue    intrusive doubly-linked list threaded through a pooled
//                 array of order nodes (32-bit indices, not pointers).
//   order index   FlatIdMap by default (open addressing), pluggable.
//
// Steady state performs zero heap allocations: the pool and index are sized
// up front and recycle through free lists.
//
// Trade-off: memory is O(band width), not O(live levels) — 8,001 levels per
// side for a +/-20% band on a $100 stock is ~190 KB/side. That is fine for a
// bounded instrument universe and is exactly how venue-side books handle
// bounded tick ranges; an unbounded-price product would need a hybrid
// (array around the touch + tree for the tails).
template <class Index = FlatIndex>
class ArrayOrderBook {
 public:
  explicit ArrayOrderBook(const InstrumentSpec& spec, std::size_t expected_orders = 1024)
      : spec_(spec),
        num_levels_(spec.num_levels()),
        bids_(num_levels_),
        asks_(num_levels_),
        bid_bits_((num_levels_ + 63) / 64, 0),
        ask_bits_((num_levels_ + 63) / 64, 0),
        index_(expected_orders) {
    nodes_.reserve(expected_orders);
  }

  BookResult add(OrderId id, Side side, Price price, Qty qty) {
    if (qty == 0) return BookResult::BadQty;
    if (!spec_.in_band(price) || !spec_.on_tick(price)) return BookResult::BadPrice;
    if (id == 0) return BookResult::DuplicateOrderId;
    const std::uint32_t n = alloc_node();
    if (!index_.insert(id, n)) {
      free_node(n);
      return BookResult::DuplicateOrderId;
    }
    Node& node = nodes_[n];
    node.id = id;
    node.qty = qty;
    node.side = side;
    node.slot = slot_of(price);
    link_back(n);
    return BookResult::Ok;
  }

  BookResult cancel(OrderId id) {
    const std::uint32_t n = index_.find(id);
    if (n == Index::kNotFound) return BookResult::UnknownOrder;
    unlink(n);
    index_.erase(id);
    free_node(n);
    return BookResult::Ok;
  }

  BookResult modify(OrderId id, Price price, Qty qty) {
    const std::uint32_t n = index_.find(id);
    if (n == Index::kNotFound) return BookResult::UnknownOrder;
    if (qty == 0) return BookResult::BadQty;
    if (!spec_.in_band(price) || !spec_.on_tick(price)) return BookResult::BadPrice;
    Node& node = nodes_[n];
    const std::uint32_t slot = slot_of(price);
    if (slot == node.slot && qty <= node.qty) {
      levels(node.side)[slot].qty -= node.qty - qty;
      node.qty = qty;
      return BookResult::Ok;
    }
    unlink(n);
    node.qty = qty;
    node.slot = slot;
    link_back(n);
    return BookResult::Ok;
  }

  BookResult execute(OrderId id, Qty qty) {
    const std::uint32_t n = index_.find(id);
    if (n == Index::kNotFound) return BookResult::UnknownOrder;
    Node& node = nodes_[n];
    if (qty == 0 || qty > node.qty) return BookResult::BadQty;
    if (qty == node.qty) {
      unlink(n);
      index_.erase(id);
      free_node(n);
    } else {
      node.qty -= qty;
      levels(node.side)[node.slot].qty -= qty;
    }
    return BookResult::Ok;
  }

  Level best_bid() const noexcept { return level_at(Side::Buy, best_bid_); }
  Level best_ask() const noexcept { return level_at(Side::Sell, best_ask_); }

  std::size_t depth(Side side, std::span<Level> out) const noexcept {
    std::size_t k = 0;
    std::int64_t s = side == Side::Buy ? best_bid_ : best_ask_;
    while (s >= 0 && k < out.size()) {
      out[k++] = level_at(side, s);
      s = side == Side::Buy ? prev_set(bid_bits_, s - 1) : next_set(ask_bits_, s + 1);
    }
    return k;
  }

  std::size_t order_count() const noexcept { return index_.size(); }
  bool find(OrderId id) const noexcept { return index_.find(id) != Index::kNotFound; }
  bool find(OrderId id, OrderView& out) const noexcept {
    const std::uint32_t n = index_.find(id);
    if (n == Index::kNotFound) return false;
    const Node& node = nodes_[n];
    out = {node.id, node.side, price_of(node.slot), node.qty};
    return true;
  }

  std::vector<OrderView> orders_at(Side side, Price price) const {
    std::vector<OrderView> v;
    if (!spec_.in_band(price) || !spec_.on_tick(price)) return v;
    for (std::uint32_t n = levels(side)[slot_of(price)].head; n != kNil; n = nodes_[n].next) {
      v.push_back({nodes_[n].id, side, price, nodes_[n].qty});
    }
    return v;
  }

  // First order in time priority at the best level of `side`.
  bool front(Side side, OrderView& out) const noexcept {
    const std::int64_t s = side == Side::Buy ? best_bid_ : best_ask_;
    if (s < 0) return false;
    const Node& node = nodes_[levels(side)[static_cast<std::size_t>(s)].head];
    out = {node.id, side, price_of(s), node.qty};
    return true;
  }

  const InstrumentSpec& spec() const noexcept { return spec_; }

  void clear() {
    for (auto& l : bids_) l = {};
    for (auto& l : asks_) l = {};
    std::fill(bid_bits_.begin(), bid_bits_.end(), 0);
    std::fill(ask_bits_.begin(), ask_bits_.end(), 0);
    nodes_.clear();
    free_head_ = kNil;
    index_.clear();
    best_bid_ = best_ask_ = -1;
  }

 private:
  static constexpr std::uint32_t kNil = 0xFFFF'FFFFu;

  struct Node {
    OrderId id = 0;
    Qty qty = 0;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;  // doubles as free-list link
    std::uint32_t slot = 0;
    Side side = Side::Buy;
  };

  struct LevelSlot {
    std::uint64_t qty = 0;
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    std::uint32_t count = 0;
  };

  std::uint32_t slot_of(Price p) const noexcept {
    return static_cast<std::uint32_t>((p - spec_.min_price) / spec_.tick_size);
  }
  Price price_of(std::int64_t slot) const noexcept { return spec_.min_price + slot * spec_.tick_size; }

  std::vector<LevelSlot>& levels(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
  const std::vector<LevelSlot>& levels(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

  Level level_at(Side side, std::int64_t slot) const noexcept {
    if (slot < 0) return {};
    const LevelSlot& l = levels(side)[static_cast<std::size_t>(slot)];
    return {price_of(slot), l.qty, l.count};
  }

  std::uint32_t alloc_node() {
    if (free_head_ != kNil) {
      const std::uint32_t n = free_head_;
      free_head_ = nodes_[n].next;
      return n;
    }
    nodes_.emplace_back();
    return static_cast<std::uint32_t>(nodes_.size() - 1);
  }

  void free_node(std::uint32_t n) noexcept {
    nodes_[n].next = free_head_;
    free_head_ = n;
  }

  static void set_bit(std::vector<std::uint64_t>& bits, std::uint32_t s) noexcept {
    bits[s >> 6] |= std::uint64_t{1} << (s & 63);
  }
  static void clear_bit(std::vector<std::uint64_t>& bits, std::uint32_t s) noexcept {
    bits[s >> 6] &= ~(std::uint64_t{1} << (s & 63));
  }

  // Highest set bit at index <= from, or -1.
  static std::int64_t prev_set(const std::vector<std::uint64_t>& bits, std::int64_t from) noexcept {
    if (from < 0) return -1;
    auto w = static_cast<std::size_t>(from >> 6);
    const auto b = static_cast<unsigned>(from & 63);
    std::uint64_t word = bits[w] & (b == 63 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (b + 1)) - 1));
    while (true) {
      if (word != 0) return static_cast<std::int64_t>(w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(word)));
      if (w == 0) return -1;
      word = bits[--w];
    }
  }

  // Lowest set bit at index >= from, or -1.
  std::int64_t next_set(const std::vector<std::uint64_t>& bits, std::int64_t from) const noexcept {
    if (from >= static_cast<std::int64_t>(num_levels_)) return -1;
    auto w = static_cast<std::size_t>(from >> 6);
    const auto b = static_cast<unsigned>(from & 63);
    std::uint64_t word = bits[w] & (~std::uint64_t{0} << b);
    while (true) {
      if (word != 0) return static_cast<std::int64_t>(w * 64 + static_cast<std::size_t>(std::countr_zero(word)));
      if (++w == bits.size()) return -1;
      word = bits[w];
    }
  }

  void link_back(std::uint32_t n) noexcept {
    Node& node = nodes_[n];
    LevelSlot& lvl = levels(node.side)[node.slot];
    node.prev = lvl.tail;
    node.next = kNil;
    if (lvl.tail != kNil) {
      nodes_[lvl.tail].next = n;
    } else {
      lvl.head = n;
    }
    lvl.tail = n;
    lvl.qty += node.qty;
    if (lvl.count++ == 0) {
      const auto s = static_cast<std::int64_t>(node.slot);
      if (node.side == Side::Buy) {
        set_bit(bid_bits_, node.slot);
        if (s > best_bid_) best_bid_ = s;
      } else {
        set_bit(ask_bits_, node.slot);
        if (best_ask_ < 0 || s < best_ask_) best_ask_ = s;
      }
    }
  }

  void unlink(std::uint32_t n) noexcept {
    Node& node = nodes_[n];
    LevelSlot& lvl = levels(node.side)[node.slot];
    if (node.prev != kNil) {
      nodes_[node.prev].next = node.next;
    } else {
      lvl.head = node.next;
    }
    if (node.next != kNil) {
      nodes_[node.next].prev = node.prev;
    } else {
      lvl.tail = node.prev;
    }
    lvl.qty -= node.qty;
    if (--lvl.count == 0) {
      const auto s = static_cast<std::int64_t>(node.slot);
      if (node.side == Side::Buy) {
        clear_bit(bid_bits_, node.slot);
        if (s == best_bid_) best_bid_ = prev_set(bid_bits_, s);
      } else {
        clear_bit(ask_bits_, node.slot);
        if (s == best_ask_) best_ask_ = next_set(ask_bits_, s);
      }
    }
  }

  InstrumentSpec spec_;
  std::uint32_t num_levels_;
  std::vector<LevelSlot> bids_;
  std::vector<LevelSlot> asks_;
  std::vector<std::uint64_t> bid_bits_;
  std::vector<std::uint64_t> ask_bits_;
  std::int64_t best_bid_ = -1;
  std::int64_t best_ask_ = -1;
  std::vector<Node> nodes_;
  std::uint32_t free_head_ = kNil;
  Index index_;
};

using FastOrderBook = ArrayOrderBook<FlatIndex>;

static_assert(OrderBook<ArrayOrderBook<FlatIndex>>);
static_assert(OrderBook<ArrayOrderBook<StdIndex>>);

}  // namespace pulse::book
