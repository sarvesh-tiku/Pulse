#pragma once

#include <cstdint>
#include <type_traits>

#include "pulse/core/types.hpp"

namespace pulse {

enum class EventType : std::uint8_t {
  Heartbeat = 0,
  AddOrder = 1,
  CancelOrder = 2,
  ModifyOrder = 3,
  Trade = 4,
  // Internal (never on the wire): feed-health transitions emitted by the
  // sequencer so downstream stages learn about gaps in-band, in order.
  FeedGap = 16,        // seq = first missing, order_id = last missing
  FeedRecovered = 17,  // gap filled; seq = next expected
  FeedStale = 18,      // gap abandoned: book state is no longer trustworthy
};

const char* to_string(EventType t) noexcept;

// Normalized, fixed-size market-data event. This is what flows through the
// pipeline's ring buffers, so it is trivially copyable and has no pointers.
// Field meaning per type:
//   AddOrder     order_id, side, price, qty
//   CancelOrder  order_id                       (removes the whole order)
//   ModifyOrder  order_id, price, qty           (new price / new total qty)
//   Trade        order_id (resting), side (resting side), price, qty (executed)
//   Heartbeat    seq = next sequence the venue will send (does not consume)
//   Feed*        see EventType
struct MarketEvent {
  SeqNum seq = 0;
  TimestampNs exchange_ts = 0;  // venue time: drives all business logic
  TimestampNs ingress_ns = 0;   // local steady clock at receipt: measurement only
  OrderId order_id = 0;
  Price price = 0;
  Qty qty = 0;
  InstrumentId instrument = 0;
  EventType type = EventType::Heartbeat;
  Side side = Side::Buy;
  std::uint16_t flags = 0;
};

static_assert(std::is_trivially_copyable_v<MarketEvent>);
static_assert(sizeof(MarketEvent) == 56);

}  // namespace pulse
