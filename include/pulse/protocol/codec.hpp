#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pulse/market_data/event.hpp"
#include "pulse/protocol/wire.hpp"

namespace pulse::protocol {

enum class DecodeStatus : std::uint8_t {
  Ok = 0,
  Truncated,    // fewer bytes than the header or than `length` claims
  BadLength,    // length field inconsistent with msg_type
  BadVersion,
  UnknownType,
  BadChecksum,
  BadField,     // side / reserved bits out of range
};

const char* to_string(DecodeStatus s) noexcept;

struct DecodeResult {
  DecodeStatus status = DecodeStatus::Truncated;
  std::size_t consumed = 0;  // bytes to skip; 0 means "need more data"
};

// Body size for a message type, or -1 if unknown.
int body_size(EventType t) noexcept;
inline std::size_t frame_size(EventType t) noexcept {
  return kHeaderSize + static_cast<std::size_t>(body_size(t));
}

// Encodes `ev` into `out` (which must have at least kMaxFrameSize bytes).
// Returns the number of bytes written. `ingress_ns` is not serialized.
std::size_t encode(const MarketEvent& ev, std::span<std::byte> out, bool with_checksum = true) noexcept;

// Decodes one frame from the front of `in`. Does no semantic validation
// (instrument ranges, tick sizes) — that is the feed handler's job.
//
// On a bad checksum or bad field, `consumed` is still the frame length so the
// caller can skip the frame and keep going; on BadLength the length itself is
// untrustworthy, so the stream cannot be resynchronised and consumed == 0.
DecodeResult decode(std::span<const std::byte> in, MarketEvent& out, bool verify_checksum = true) noexcept;

// Recomputes and patches the checksum of an already-encoded frame.
void reseal(std::span<std::byte> frame) noexcept;

}  // namespace pulse::protocol
