#include "pulse/market_data/sequencer.hpp"

#include <bit>
#include <stdexcept>

namespace pulse::md {

const char* to_string(FeedState s) noexcept {
  switch (s) {
    case FeedState::Live: return "LIVE";
    case FeedState::Recovering: return "RECOVERING";
    case FeedState::Stale: return "STALE";
  }
  return "?";
}

Sequencer::Sequencer(std::size_t window, SeqNum first_expected)
    : window_(std::bit_ceil(std::max<std::size_t>(window, 2))),
      mask_(window_.size() - 1),
      expected_(first_expected) {}

GapRange Sequencer::outstanding() const noexcept {
  if (state_ != FeedState::Recovering) return {};
  return {expected_, highest_seen_};
}

bool Sequencer::take_request(GapRange& out) noexcept {
  if (!pending_request_) return false;
  pending_request_ = false;
  out = request_;
  return true;
}

}  // namespace pulse::md
