#pragma once

#include <cstdint>

#include "pulse/core/types.hpp"

namespace pulse::exec {

using ClOrdId = std::uint64_t;    // client-assigned (strategy side)
using ExchOrdId = std::uint64_t;  // venue-assigned
using StrategyId = std::uint16_t;

enum class TimeInForce : std::uint8_t { Day, IOC };

enum class RequestType : std::uint8_t { New, Cancel, Replace };

// Client -> venue.
struct OrderRequest {
  RequestType type = RequestType::New;
  ClOrdId cl_id = 0;
  ClOrdId orig_cl_id = 0;  // Cancel / Replace: the order being acted on
  InstrumentId instrument = 0;
  Side side = Side::Buy;
  TimeInForce tif = TimeInForce::Day;
  StrategyId strategy = 0;
  Price price = 0;
  Qty qty = 0;
  TimestampNs sent_ts = 0;  // simulated time the request left the client
};

enum class ExecType : std::uint8_t {
  Accepted,
  Rejected,
  PartialFill,
  Fill,
  Cancelled,
  Replaced,
  CancelRejected,
  SessionDown,  // venue connection lost (unsolicited)
  SessionUp,    // venue connection restored (unsolicited)
};

const char* to_string(ExecType t) noexcept;

enum class RejectReason : std::uint8_t {
  None,
  InvalidPrice,
  InvalidQty,
  UnknownInstrument,
  UnknownOrder,
  VenueBusy,       // injected random reject
  TooLateToCancel,
};

const char* to_string(RejectReason r) noexcept;

// Venue -> client.
struct ExecReport {
  ExecType type = ExecType::Accepted;
  RejectReason reason = RejectReason::None;
  Side side = Side::Buy;
  InstrumentId instrument = 0;
  ClOrdId cl_id = 0;
  ExchOrdId exch_id = 0;
  std::uint64_t exec_id = 0;  // unique per report, monotonically assigned by the venue
  Price price = 0;            // order price (Accepted/Replaced) or fill price
  Qty last_qty = 0;           // fill quantity
  Qty leaves_qty = 0;
  Qty cum_qty = 0;
  TimestampNs venue_ts = 0;    // when the venue generated it
  TimestampNs deliver_ts = 0;  // when the client receives it
};

}  // namespace pulse::exec
