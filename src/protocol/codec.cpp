#include "pulse/protocol/codec.hpp"

#include <cstring>

#include "pulse/protocol/checksum.hpp"

namespace pulse::protocol {
namespace {

template <class T>
inline void put(std::byte* base, std::size_t offset, T v) noexcept {
  std::memcpy(base + offset, &v, sizeof(T));
}

template <class T>
inline T get(const std::byte* base, std::size_t offset) noexcept {
  T v;
  std::memcpy(&v, base + offset, sizeof(T));
  return v;
}

inline std::uint32_t frame_checksum(const std::byte* p, std::size_t len) noexcept {
  // Skip the 4-byte checksum field itself instead of zeroing it in a copy.
  std::uint32_t crc = crc32c(0, {p, kChecksumOffset});
  return crc32c(crc, {p + kChecksumOffset + 4, len - kChecksumOffset - 4});
}

}  // namespace

const char* to_string(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::Ok: return "ok";
    case DecodeStatus::Truncated: return "truncated";
    case DecodeStatus::BadLength: return "bad_length";
    case DecodeStatus::BadVersion: return "bad_version";
    case DecodeStatus::UnknownType: return "unknown_type";
    case DecodeStatus::BadChecksum: return "bad_checksum";
    case DecodeStatus::BadField: return "bad_field";
  }
  return "?";
}

int body_size(EventType t) noexcept {
  switch (t) {
    case EventType::Heartbeat: return static_cast<int>(kHeartbeatBodySize);
    case EventType::AddOrder: return static_cast<int>(kAddBodySize);
    case EventType::CancelOrder: return static_cast<int>(kCancelBodySize);
    case EventType::ModifyOrder: return static_cast<int>(kModifyBodySize);
    case EventType::Trade: return static_cast<int>(kTradeBodySize);
    default: return -1;
  }
}

std::size_t encode(const MarketEvent& ev, std::span<std::byte> out, bool with_checksum) noexcept {
  const std::size_t len = frame_size(ev.type);
  std::byte* p = out.data();
  std::memset(p, 0, len);
  put<std::uint16_t>(p, off::length, static_cast<std::uint16_t>(len));
  put<std::uint8_t>(p, off::type, static_cast<std::uint8_t>(ev.type));
  put<std::uint8_t>(p, off::version, kProtocolVersion);
  put<std::uint32_t>(p, off::instrument, ev.instrument);
  put<std::uint64_t>(p, off::sequence, ev.seq);
  put<std::uint64_t>(p, off::timestamp, ev.exchange_ts);
  auto flags = static_cast<std::uint16_t>(ev.flags & ~kFlagNoChecksum);
  if (!with_checksum) flags = static_cast<std::uint16_t>(flags | kFlagNoChecksum);
  put<std::uint16_t>(p, off::flags, flags);

  std::byte* b = p + off::body;
  switch (ev.type) {
    case EventType::Heartbeat: break;
    case EventType::AddOrder:
    case EventType::Trade:
      put<std::uint64_t>(b, 0, ev.order_id);
      put<std::int64_t>(b, 8, ev.price);
      put<std::uint32_t>(b, 16, ev.qty);
      put<std::uint8_t>(b, 20, static_cast<std::uint8_t>(ev.side));
      break;
    case EventType::CancelOrder:
      put<std::uint64_t>(b, 0, ev.order_id);
      break;
    case EventType::ModifyOrder:
      put<std::uint64_t>(b, 0, ev.order_id);
      put<std::int64_t>(b, 8, ev.price);
      put<std::uint32_t>(b, 16, ev.qty);
      break;
    default: break;
  }
  if (with_checksum) put<std::uint32_t>(p, off::checksum, frame_checksum(p, len));
  return len;
}

void reseal(std::span<std::byte> frame) noexcept {
  const auto len = get<std::uint16_t>(frame.data(), off::length);
  put<std::uint32_t>(frame.data(), off::checksum, frame_checksum(frame.data(), len));
}

DecodeResult decode(std::span<const std::byte> in, MarketEvent& out, bool verify_checksum) noexcept {
  if (in.size() < kHeaderSize) return {DecodeStatus::Truncated, 0};
  const std::byte* p = in.data();
  const auto len = get<std::uint16_t>(p, off::length);
  const auto raw_type = get<std::uint8_t>(p, off::type);

  if (raw_type > static_cast<std::uint8_t>(EventType::Trade)) {
    // Type is garbage, but if the length looks like a legal frame we can still
    // skip it; otherwise the stream is unsynchronised.
    const bool plausible = len >= kHeaderSize && len <= kMaxFrameSize && len % 8 == 0 && len <= in.size();
    return {DecodeStatus::UnknownType, plausible ? len : std::size_t{0}};
  }
  const auto type = static_cast<EventType>(raw_type);
  if (len != frame_size(type)) return {DecodeStatus::BadLength, 0};
  if (in.size() < len) return {DecodeStatus::Truncated, 0};
  if (get<std::uint8_t>(p, off::version) != kProtocolVersion) return {DecodeStatus::BadVersion, len};

  const auto flags = get<std::uint16_t>(p, off::flags);
  if (verify_checksum) {
    const auto sum = get<std::uint32_t>(p, off::checksum);
    if ((flags & kFlagNoChecksum) == 0) {
      if (sum != frame_checksum(p, len)) return {DecodeStatus::BadChecksum, len};
    } else if (sum != 0) {
      // An unchecksummed frame always carries 0 here. Without this rule a
      // single bit flip in `flags` could switch verification off.
      return {DecodeStatus::BadChecksum, len};
    }
  }
  if (get<std::uint16_t>(p, off::reserved) != 0) return {DecodeStatus::BadField, len};

  out.type = type;
  out.instrument = get<std::uint32_t>(p, off::instrument);
  out.seq = get<std::uint64_t>(p, off::sequence);
  out.exchange_ts = get<std::uint64_t>(p, off::timestamp);
  out.flags = flags;
  out.order_id = 0;
  out.price = 0;
  out.qty = 0;
  out.side = Side::Buy;

  const std::byte* b = p + off::body;
  switch (type) {
    case EventType::Heartbeat: break;
    case EventType::AddOrder:
    case EventType::Trade: {
      out.order_id = get<std::uint64_t>(b, 0);
      out.price = get<std::int64_t>(b, 8);
      out.qty = get<std::uint32_t>(b, 16);
      const auto side = get<std::uint8_t>(b, 20);
      if (side > 1) return {DecodeStatus::BadField, len};
      out.side = static_cast<Side>(side);
      break;
    }
    case EventType::CancelOrder:
      out.order_id = get<std::uint64_t>(b, 0);
      break;
    case EventType::ModifyOrder:
      out.order_id = get<std::uint64_t>(b, 0);
      out.price = get<std::int64_t>(b, 8);
      out.qty = get<std::uint32_t>(b, 16);
      break;
    default: break;
  }
  return {DecodeStatus::Ok, len};
}

}  // namespace pulse::protocol

namespace pulse {
const char* to_string(EventType t) noexcept {
  switch (t) {
    case EventType::Heartbeat: return "HEARTBEAT";
    case EventType::AddOrder: return "ADD";
    case EventType::CancelOrder: return "CANCEL";
    case EventType::ModifyOrder: return "MODIFY";
    case EventType::Trade: return "TRADE";
    case EventType::FeedGap: return "FEED_GAP";
    case EventType::FeedRecovered: return "FEED_RECOVERED";
    case EventType::FeedStale: return "FEED_STALE";
  }
  return "?";
}
}  // namespace pulse
