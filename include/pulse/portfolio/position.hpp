#pragma once

#include <cstdint>
#include <vector>

#include "pulse/core/types.hpp"

namespace pulse::portfolio {

// Money is kept in the same fixed-point scale as prices (1e-4) multiplied by
// shares, in int64: 1e6 shares at $10,000 is 1e14 -- far from overflow.
using Money = std::int64_t;

// Average-cost position accounting for one instrument.
//
//   open_cost   sum of price*qty over the currently open quantity
//   realized    booked when a fill reduces the position:
//                 long  closed by a sell: proceeds - removed_cost
//                 short closed by a buy : removed_cost - cost_to_buy_back
//   unrealized  marked to a reference price (mid):
//                 long : mark*qty - open_cost
//                 short: open_cost - mark*|qty|
// A fill that flips the position closes the old side completely and opens
// the remainder at the fill price.
class Position {
 public:
  void apply_fill(Side side, Price px, Qty qty) noexcept;

  std::int64_t qty() const noexcept { return qty_; }
  Money open_cost() const noexcept { return open_cost_; }
  Money realized() const noexcept { return realized_; }
  Money unrealized(Price mark) const noexcept;
  // Average entry price (rounded toward zero); 0 when flat.
  Price avg_price() const noexcept;
  std::uint64_t bought() const noexcept { return bought_; }
  std::uint64_t sold() const noexcept { return sold_; }
  std::uint64_t fills() const noexcept { return fills_; }

 private:
  std::int64_t qty_ = 0;
  Money open_cost_ = 0;
  Money realized_ = 0;
  std::uint64_t bought_ = 0;
  std::uint64_t sold_ = 0;
  std::uint64_t fills_ = 0;
};

class Portfolio {
 public:
  explicit Portfolio(std::size_t instruments) : positions_(instruments), marks_(instruments, kInvalidPrice) {}

  void apply_fill(InstrumentId inst, Side side, Price px, Qty qty) noexcept {
    positions_[inst].apply_fill(side, px, qty);
  }
  void mark(InstrumentId inst, Price px) noexcept {
    if (px != kInvalidPrice) marks_[inst] = px;
  }

  const Position& operator[](InstrumentId i) const noexcept { return positions_[i]; }
  Price mark_of(InstrumentId i) const noexcept { return marks_[i]; }
  std::size_t size() const noexcept { return positions_.size(); }

  Money realized() const noexcept;
  Money unrealized() const noexcept;
  Money total() const noexcept { return realized() + unrealized(); }
  // Sum over instruments of |position| * mark.
  Money gross_exposure() const noexcept;

 private:
  std::vector<Position> positions_;
  std::vector<Price> marks_;
};

}  // namespace pulse::portfolio
