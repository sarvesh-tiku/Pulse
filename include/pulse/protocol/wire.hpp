#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

// Pulse Market Data Protocol (PMDP) v1 — wire layout.
//
// Every frame is self-delimiting and little-endian:
//
//   offset size field
//   0      2    length        total frame length in bytes, header included
//   2      1    msg_type      EventType
//   3      1    version       kProtocolVersion
//   4      4    instrument_id
//   8      8    sequence      per-feed, gap-free, starts at 1
//   16     8    exchange_ts   venue nanoseconds
//   24     4    checksum      CRC32C over bytes [0,24) ++ [28,length)
//   28     2    flags         kFlag*
//   30     2    reserved      must be zero
//   32     ..   body          type-specific, fixed size per type
//
// Bodies (all 8-byte multiples so frames stay 8-byte aligned in a buffer):
//   AddOrder    24: order_id u64 | price i64 | qty u32 | side u8 | pad[3]
//   CancelOrder  8: order_id u64
//   ModifyOrder 24: order_id u64 | new_price i64 | new_qty u32 | pad[4]
//   Trade       24: order_id u64 | price i64 | qty u32 | resting_side u8 | pad[3]
//   Heartbeat    0: (sequence = next sequence to be sent; does not consume one)
//
// Fixed per-type sizes let the decoder reject a malformed length before it
// touches the body, and make a corrupted length field detectable without
// trusting it.

namespace pulse::protocol {

static_assert(std::endian::native == std::endian::little,
              "PMDP encoder/decoder assume a little-endian host");

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 32;
inline constexpr std::size_t kChecksumOffset = 24;
inline constexpr std::size_t kMaxFrameSize = 64;

inline constexpr std::uint16_t kFlagRetransmit = 1u << 0;  // sent by the rewind server
inline constexpr std::uint16_t kFlagNoChecksum = 1u << 1;  // checksum field not populated

namespace off {
inline constexpr std::size_t length = 0;
inline constexpr std::size_t type = 2;
inline constexpr std::size_t version = 3;
inline constexpr std::size_t instrument = 4;
inline constexpr std::size_t sequence = 8;
inline constexpr std::size_t timestamp = 16;
inline constexpr std::size_t checksum = 24;
inline constexpr std::size_t flags = 28;
inline constexpr std::size_t reserved = 30;
inline constexpr std::size_t body = 32;
}  // namespace off

inline constexpr std::size_t kAddBodySize = 24;
inline constexpr std::size_t kCancelBodySize = 8;
inline constexpr std::size_t kModifyBodySize = 24;
inline constexpr std::size_t kTradeBodySize = 24;
inline constexpr std::size_t kHeartbeatBodySize = 0;

}  // namespace pulse::protocol
