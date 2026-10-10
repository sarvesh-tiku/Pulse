#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pulse/core/types.hpp"
#include "pulse/market_data/session_file.hpp"

namespace pulse::md {

// Source of retransmissions for gap recovery (the "rewind" / "replay" service
// real venues run alongside their multicast feeds).
class RetransmitSource {
 public:
  virtual ~RetransmitSource() = default;
  // Appends frames with sequence numbers [first, last] (in order, flagged as
  // retransmissions) to `out`. Returns false if the range is unavailable.
  virtual bool fetch(SeqNum first, SeqNum last, std::vector<std::byte>& out) = 0;
  virtual const char* name() const noexcept = 0;
};

// Serves retransmissions from a clean session (what the venue actually sent).
// Builds a dense seq -> frame index on construction.
class SessionRewindStore final : public RetransmitSource {
 public:
  explicit SessionRewindStore(SessionReader reader);
  static std::unique_ptr<SessionRewindStore> open(const std::string& path);

  bool fetch(SeqNum first, SeqNum last, std::vector<std::byte>& out) override;
  const char* name() const noexcept override { return "session-rewind"; }
  SeqNum max_seq() const noexcept { return static_cast<SeqNum>(index_.size()); }
  // Frame for a single sequence, or empty span.
  std::span<const std::byte> frame(SeqNum seq) const noexcept;

 private:
  SessionReader reader_;
  std::vector<std::span<const std::byte>> index_;  // index_[seq - 1]
};

}  // namespace pulse::md
