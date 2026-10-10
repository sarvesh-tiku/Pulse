#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "pulse/market_data/event.hpp"

namespace pulse::md {

enum class FeedState : std::uint8_t {
  Live,        // in sequence
  Recovering,  // gap outstanding; later messages are buffered
  Stale,       // a gap was abandoned; downstream state cannot be trusted
};

const char* to_string(FeedState s) noexcept;

struct SequencerStats {
  std::uint64_t delivered = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t gaps = 0;             // gap episodes detected
  std::uint64_t missing = 0;          // total sequence numbers found missing
  std::uint64_t recovered = 0;        // missing messages later filled
  std::uint64_t buffered_peak = 0;
  std::uint64_t abandoned = 0;        // messages given up on (Stale)
};

struct GapRange {
  SeqNum first = 0;
  SeqNum last = 0;
};

// Enforces gap-free, in-order delivery of a sequenced feed.
//
//   seq <  expected  -> duplicate (A/B arbitration, retransmit overlap): drop
//   seq == expected  -> deliver, then drain any buffered successors
//   seq >  expected  -> gap: buffer the message, report the missing range,
//                       stay in Recovering until it is filled
//
// Buffered messages live in a fixed power-of-two window indexed by
// seq & (W - 1) (no allocation per message). If a message arrives beyond the
// window, or the caller gives up on recovery, the sequencer skips ahead and
// enters Stale: downstream books may be missing messages.
//
// Output is pushed into the caller's sink as MarketEvents, including the
// in-band FeedGap / FeedRecovered / FeedStale status events.
class Sequencer {
 public:
  explicit Sequencer(std::size_t window = 1 << 16, SeqNum first_expected = 1);

  template <class Sink>
  void on_message(const MarketEvent& ev, Sink&& sink);

  // Abandons the outstanding gap: skip to the lowest buffered sequence.
  template <class Sink>
  void give_up(Sink&& sink);

  FeedState state() const noexcept { return state_; }
  SeqNum expected() const noexcept { return expected_; }
  // Missing range still outstanding (valid while Recovering).
  GapRange outstanding() const noexcept;
  // Set by on_message when a *new* gap needs a retransmission request.
  bool take_request(GapRange& out) noexcept;
  const SequencerStats& stats() const noexcept { return stats_; }

 private:
  struct Slot {
    MarketEvent ev;
    bool used = false;
  };

  template <class Sink>
  void deliver(const MarketEvent& ev, Sink& sink);
  template <class Sink>
  void drain(Sink& sink);
  template <class Sink>
  void emit_status(EventType t, SeqNum seq, SeqNum last, TimestampNs ts, Sink& sink);

  std::vector<Slot> window_;
  std::size_t mask_;
  SeqNum expected_;
  SeqNum highest_seen_ = 0;  // highest sequence buffered or heartbeat-announced
  std::size_t buffered_ = 0;
  FeedState state_ = FeedState::Live;
  bool pending_request_ = false;
  GapRange request_{};
  TimestampNs last_ts_ = 0;
  SequencerStats stats_;
};

// ---------------------------------------------------------------------------

template <class Sink>
void Sequencer::emit_status(EventType t, SeqNum seq, SeqNum last, TimestampNs ts, Sink& sink) {
  MarketEvent s{};
  s.type = t;
  s.seq = seq;
  s.order_id = last;
  s.exchange_ts = ts;
  sink(s);
}

template <class Sink>
void Sequencer::deliver(const MarketEvent& ev, Sink& sink) {
  ++stats_.delivered;
  last_ts_ = ev.exchange_ts;
  ++expected_;
  sink(ev);
}

template <class Sink>
void Sequencer::drain(Sink& sink) {
  while (buffered_ > 0) {
    Slot& s = window_[expected_ & mask_];
    if (!s.used || s.ev.seq != expected_) break;
    s.used = false;
    --buffered_;
    deliver(s.ev, sink);
  }
  if (state_ == FeedState::Recovering && expected_ > highest_seen_) {
    state_ = FeedState::Live;
    emit_status(EventType::FeedRecovered, expected_, 0, last_ts_, sink);
  }
}

template <class Sink>
void Sequencer::on_message(const MarketEvent& ev, Sink&& sink) {
  if (ev.type == EventType::Heartbeat) {
    // A heartbeat announces the next sequence; anything before it is missing.
    if (ev.seq > expected_ && ev.seq - 1 > highest_seen_) {
      const SeqNum first = std::max(expected_, highest_seen_ + 1);
      const SeqNum last = ev.seq - 1;
      if (state_ == FeedState::Live) {
        state_ = FeedState::Recovering;
        ++stats_.gaps;
        emit_status(EventType::FeedGap, expected_, last, ev.exchange_ts, sink);
      }
      stats_.missing += last - first + 1;
      highest_seen_ = last;
      pending_request_ = true;
      request_ = {first, last};
    }
    return;
  }

  if (ev.seq < expected_) {
    ++stats_.duplicates;
    return;
  }
  if (ev.seq == expected_) {
    // While recovering, `expected_` is by definition a missing number.
    const bool was_gap_fill = state_ == FeedState::Recovering;
    if (was_gap_fill) ++stats_.recovered;
    deliver(ev, sink);
    if (was_gap_fill || buffered_ > 0) drain(sink);
    return;
  }

  // Gap: ev.seq > expected_.
  if (ev.seq - expected_ >= window_.size()) {
    // Too far ahead to buffer: abandon everything up to here.
    stats_.abandoned += ev.seq - expected_ - buffered_;
    for (auto& s : window_) s.used = false;
    buffered_ = 0;
    state_ = FeedState::Stale;
    emit_status(EventType::FeedStale, expected_, ev.seq - 1, ev.exchange_ts, sink);
    expected_ = ev.seq;
    highest_seen_ = ev.seq;
    pending_request_ = false;
    deliver(ev, sink);
    return;
  }

  Slot& slot = window_[ev.seq & mask_];
  if (slot.used && slot.ev.seq == ev.seq) {
    ++stats_.duplicates;
    return;
  }
  slot.ev = ev;
  slot.used = true;
  ++buffered_;
  // A message inside the known-missing range fills a hole out of position.
  if (ev.seq <= highest_seen_) ++stats_.recovered;
  stats_.buffered_peak = std::max<std::uint64_t>(stats_.buffered_peak, buffered_);

  if (ev.seq > highest_seen_) {
    // New missing numbers: (max(expected, highest_seen+1) .. ev.seq-1).
    const SeqNum first = std::max(expected_, highest_seen_ + 1);
    if (ev.seq > first) {
      stats_.missing += ev.seq - first;
      pending_request_ = true;
      request_ = {first, ev.seq - 1};
    }
    highest_seen_ = ev.seq;
  }
  if (state_ != FeedState::Recovering) {
    state_ = FeedState::Recovering;
    ++stats_.gaps;
    emit_status(EventType::FeedGap, expected_, ev.seq - 1, ev.exchange_ts, sink);
  }
}

template <class Sink>
void Sequencer::give_up(Sink&& sink) {
  if (state_ != FeedState::Recovering) return;
  // Skip to the lowest buffered message (or past the announced range).
  SeqNum next = highest_seen_ + 1;
  for (SeqNum s = expected_; s <= highest_seen_; ++s) {
    const Slot& slot = window_[s & mask_];
    if (slot.used && slot.ev.seq == s) {
      next = s;
      break;
    }
  }
  stats_.abandoned += next - expected_;
  state_ = FeedState::Stale;
  emit_status(EventType::FeedStale, expected_, next - 1, last_ts_, sink);
  expected_ = next;
  pending_request_ = false;
  drain(sink);
  // drain() only reports recovery from Recovering; Stale is sticky by design.
}

}  // namespace pulse::md
