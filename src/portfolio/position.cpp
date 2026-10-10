#include "pulse/portfolio/position.hpp"

#include <algorithm>
#include <cstdlib>

namespace pulse::portfolio {

void Position::apply_fill(Side side, Price px, Qty qty) noexcept {
  if (qty == 0) return;
  ++fills_;
  (side == Side::Buy ? bought_ : sold_) += qty;
  const std::int64_t signed_qty = side == Side::Buy ? static_cast<std::int64_t>(qty) : -static_cast<std::int64_t>(qty);
  const Money notional = px * static_cast<Money>(qty);

  // Opening or adding to the same side.
  if (qty_ == 0 || (qty_ > 0) == (signed_qty > 0)) {
    qty_ += signed_qty;
    open_cost_ += notional;
    return;
  }

  // Reducing (and possibly flipping).
  const std::int64_t open_abs = std::llabs(qty_);
  const std::int64_t close = std::min<std::int64_t>(open_abs, static_cast<std::int64_t>(qty));
  // Cost of the closed part at average cost. Integer division rounds toward
  // zero; the remainder stays in open_cost_ so nothing is lost -- when the
  // position is closed fully, removed_cost == open_cost_ exactly.
  const Money removed_cost = close == open_abs ? open_cost_ : open_cost_ / open_abs * close +
                                                                   (open_cost_ % open_abs) * close / open_abs;
  const Money close_value = px * close;
  realized_ += qty_ > 0 ? close_value - removed_cost : removed_cost - close_value;
  open_cost_ -= removed_cost;
  qty_ += qty_ > 0 ? -close : close;

  const std::int64_t remainder = static_cast<std::int64_t>(qty) - close;
  if (remainder > 0) {  // flipped: open the rest on the new side
    qty_ = signed_qty > 0 ? remainder : -remainder;
    open_cost_ = px * remainder;
  }
}

Money Position::unrealized(Price mark) const noexcept {
  if (qty_ == 0 || mark == kInvalidPrice) return 0;
  const Money value = mark * std::llabs(qty_);
  return qty_ > 0 ? value - open_cost_ : open_cost_ - value;
}

Price Position::avg_price() const noexcept {
  if (qty_ == 0) return 0;
  return open_cost_ / std::llabs(qty_);
}

Money Portfolio::realized() const noexcept {
  Money m = 0;
  for (const auto& p : positions_) m += p.realized();
  return m;
}

Money Portfolio::unrealized() const noexcept {
  Money m = 0;
  for (std::size_t i = 0; i < positions_.size(); ++i) m += positions_[i].unrealized(marks_[i]);
  return m;
}

Money Portfolio::gross_exposure() const noexcept {
  Money m = 0;
  for (std::size_t i = 0; i < positions_.size(); ++i) {
    if (marks_[i] != kInvalidPrice) m += std::llabs(positions_[i].qty()) * marks_[i];
  }
  return m;
}

}  // namespace pulse::portfolio
