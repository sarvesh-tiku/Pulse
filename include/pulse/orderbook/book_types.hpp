#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <vector>

#include "pulse/core/instrument.hpp"
#include "pulse/core/types.hpp"

namespace pulse::book {

enum class BookResult : std::uint8_t {
  Ok = 0,
  DuplicateOrderId,
  UnknownOrder,
  BadPrice,   // off tick or outside the instrument's band
  BadQty,     // zero quantity, or execution larger than the resting quantity
};

const char* to_string(BookResult r) noexcept;

struct Level {
  Price price = kInvalidPrice;
  std::uint64_t qty = 0;     // aggregate displayed quantity
  std::uint32_t orders = 0;  // number of resting orders
  friend bool operator==(const Level&, const Level&) = default;
};

struct OrderView {
  OrderId id = 0;
  Side side = Side::Buy;
  Price price = 0;
  Qty qty = 0;
};

// The interface every book implementation satisfies. Books are *passive*:
// they reconstruct venue state from an L3 (order-by-order) feed; they do not
// match. Semantics (identical across implementations, enforced by tests):
//
//  add      rejects zero qty, then off-tick / out-of-band prices, then
//           duplicate (or zero) ids — in that precedence; appends to the back of the price level (time priority).
//  cancel   removes the whole order.
//  modify   same price and qty <= current  -> reduce in place, keep priority;
//           anything else                   -> lose priority (move to back of
//                                              the new level). qty 0 is BadQty.
//  execute  reduces the resting order; removes it when fully filled.
template <class B>
concept OrderBook = requires(B b, const B cb, OrderId id, Side s, Price p, Qty q,
                             std::span<Level> out) {
  { b.add(id, s, p, q) } -> std::same_as<BookResult>;
  { b.cancel(id) } -> std::same_as<BookResult>;
  { b.modify(id, p, q) } -> std::same_as<BookResult>;
  { b.execute(id, q) } -> std::same_as<BookResult>;
  { cb.best_bid() } -> std::same_as<Level>;
  { cb.best_ask() } -> std::same_as<Level>;
  { cb.depth(s, out) } -> std::same_as<std::size_t>;
  { cb.order_count() } -> std::same_as<std::size_t>;
  { cb.find(id) } -> std::same_as<bool>;
  { cb.orders_at(s, p) } -> std::same_as<std::vector<OrderView>>;
};

}  // namespace pulse::book
