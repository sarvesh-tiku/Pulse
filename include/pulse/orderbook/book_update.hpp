#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "pulse/core/types.hpp"
#include "pulse/market_data/event.hpp"

namespace pulse {

inline constexpr std::size_t kBookDepth = 5;

// What the book stage publishes to the trading core: an L2 snapshot of the
// top kBookDepth levels after applying one market event, plus the trade (if
// the event was one) and feed health. Fixed-size and trivially copyable so it
// can travel through the SPSC ring by value.
struct alignas(64) BookUpdate {
  SeqNum seq = 0;
  TimestampNs exchange_ts = 0;
  TimestampNs ingress_ns = 0;  // measurement: when the frame was received
  TimestampNs book_ns = 0;     // measurement: when the book stage published
  InstrumentId instrument = 0;
  EventType cause = EventType::Heartbeat;
  std::uint8_t flags = 0;
  std::uint8_t bid_levels = 0;
  std::uint8_t ask_levels = 0;

  std::array<Price, kBookDepth> bid_px{};
  std::array<Price, kBookDepth> ask_px{};
  std::array<std::uint32_t, kBookDepth> bid_qty{};
  std::array<std::uint32_t, kBookDepth> ask_qty{};

  Price trade_px = 0;
  Qty trade_qty = 0;  // for a FeedGap status update: number of missing messages
  Side trade_resting_side = Side::Buy;

  static constexpr std::uint8_t kFlagTrade = 1u << 0;
  static constexpr std::uint8_t kFlagFeedGap = 1u << 1;        // gap open: book is behind
  static constexpr std::uint8_t kFlagFeedRecovered = 1u << 2;
  static constexpr std::uint8_t kFlagFeedStale = 1u << 3;      // sticky: book untrustworthy
  static constexpr std::uint8_t kFlagCrossed = 1u << 4;        // bid >= ask observed
  static constexpr std::uint8_t kFlagStatusOnly = 1u << 5;     // no instrument payload

  Price best_bid() const noexcept { return bid_levels ? bid_px[0] : kInvalidPrice; }
  Price best_ask() const noexcept { return ask_levels ? ask_px[0] : kInvalidPrice; }
  bool two_sided() const noexcept { return bid_levels && ask_levels; }
  // Mid in price units (rounded down); kInvalidPrice if one-sided.
  Price mid() const noexcept { return two_sided() ? (bid_px[0] + ask_px[0]) / 2 : kInvalidPrice; }
  bool has(std::uint8_t f) const noexcept { return (flags & f) != 0; }
};

static_assert(std::is_trivially_copyable_v<BookUpdate>);

}  // namespace pulse
