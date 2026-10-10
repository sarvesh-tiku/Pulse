#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pulse/core/types.hpp"

namespace pulse {

// Static reference data for one instrument. The price band is what makes the
// price-indexed order book possible: every legal price maps to a slot
// (price - min_price) / tick_size. Real venues publish similar limit-up /
// limit-down bands; the generator never walks outside them.
struct InstrumentSpec {
  InstrumentId id = 0;
  std::array<char, 8> symbol{};  // NUL-padded
  Price tick_size = 100;         // 0.01
  Price min_price = 0;
  Price max_price = 0;
  Price reference_price = 0;

  std::string_view symbol_view() const noexcept;
  bool on_tick(Price p) const noexcept { return p % tick_size == 0; }
  bool in_band(Price p) const noexcept { return p >= min_price && p <= max_price; }
  std::uint32_t num_levels() const noexcept {
    return static_cast<std::uint32_t>((max_price - min_price) / tick_size) + 1;
  }
};

InstrumentSpec make_instrument(InstrumentId id, std::string_view symbol, Price reference_price,
                               Price tick_size = 100, double band_fraction = 0.20);

class InstrumentUniverse {
 public:
  InstrumentUniverse() = default;
  explicit InstrumentUniverse(std::vector<InstrumentSpec> specs);

  // n synthetic instruments with deterministic symbols and reference prices.
  static InstrumentUniverse synthetic(std::size_t n);

  std::size_t size() const noexcept { return specs_.size(); }
  const InstrumentSpec& operator[](InstrumentId id) const noexcept { return specs_[id]; }
  bool contains(InstrumentId id) const noexcept { return id < specs_.size(); }
  const std::vector<InstrumentSpec>& specs() const noexcept { return specs_; }
  std::optional<InstrumentId> find(std::string_view symbol) const noexcept;

 private:
  std::vector<InstrumentSpec> specs_;  // dense: specs_[i].id == i
};

std::string format_price(Price p);

}  // namespace pulse
