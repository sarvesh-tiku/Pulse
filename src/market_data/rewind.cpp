#include "pulse/market_data/rewind.hpp"

#include <cstring>

#include "pulse/protocol/codec.hpp"

namespace pulse::md {

SessionRewindStore::SessionRewindStore(SessionReader reader) : reader_(std::move(reader)) {
  reader_.rewind();
  Record r;
  MarketEvent ev;
  while (reader_.next(r)) {
    std::size_t pos = 0;
    while (pos < r.frame.size()) {
      const auto res = protocol::decode(r.frame.subspan(pos), ev);
      if (res.status != protocol::DecodeStatus::Ok) break;
      if (ev.type != EventType::Heartbeat && ev.seq >= 1) {
        if (index_.size() < ev.seq) index_.resize(ev.seq);
        index_[ev.seq - 1] = r.frame.subspan(pos, res.consumed);
      }
      pos += res.consumed;
    }
  }
  reader_.rewind();
}

std::unique_ptr<SessionRewindStore> SessionRewindStore::open(const std::string& path) {
  return std::make_unique<SessionRewindStore>(SessionReader::open(path));
}

std::span<const std::byte> SessionRewindStore::frame(SeqNum seq) const noexcept {
  if (seq == 0 || seq > index_.size()) return {};
  return index_[seq - 1];
}

bool SessionRewindStore::fetch(SeqNum first, SeqNum last, std::vector<std::byte>& out) {
  if (first == 0 || last < first || last > index_.size()) return false;
  for (SeqNum s = first; s <= last; ++s) {
    const auto f = index_[s - 1];
    if (f.empty()) return false;
    const auto at = out.size();
    out.insert(out.end(), f.begin(), f.end());
    // Mark as retransmission and re-seal the checksum.
    std::span<std::byte> copy{out.data() + at, f.size()};
    std::uint16_t flags;
    std::memcpy(&flags, copy.data() + protocol::off::flags, 2);
    flags = static_cast<std::uint16_t>(flags | protocol::kFlagRetransmit);
    std::memcpy(copy.data() + protocol::off::flags, &flags, 2);
    if ((flags & protocol::kFlagNoChecksum) == 0) protocol::reseal(copy);
  }
  return true;
}

}  // namespace pulse::md
