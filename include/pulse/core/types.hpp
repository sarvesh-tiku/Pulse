#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace pulse {

// Prices are fixed-point integers: 1 unit = 1e-4 of the quote currency
// (the same scale NASDAQ ITCH uses). Floating point never touches the book,
// the matching logic or the P&L ledger; it only appears when formatting.
using Price = std::int64_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;
using InstrumentId = std::uint32_t;
using SeqNum = std::uint64_t;
using TimestampNs = std::uint64_t;

inline constexpr Price kPriceScale = 10'000;
inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr std::string_view to_string(Side s) noexcept { return s == Side::Buy ? "BUY" : "SELL"; }
constexpr std::int64_t sign(Side s) noexcept { return s == Side::Buy ? 1 : -1; }

constexpr double to_double(Price p) noexcept {
  return static_cast<double>(p) / static_cast<double>(kPriceScale);
}

}  // namespace pulse
