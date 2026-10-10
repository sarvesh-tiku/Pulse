#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "pulse/core/instrument.hpp"
#include "pulse/market_data/rewind.hpp"
#include "pulse/market_data/sequencer.hpp"
#include "pulse/protocol/codec.hpp"

namespace pulse::md {

struct FeedHandlerConfig {
  bool verify_checksum = true;
  std::size_t reorder_window = 1 << 16;
  // Passive mode (no retransmit source, e.g. replaying a recording): how many
  // further messages may arrive while a gap is open before giving up on it.
  std::uint64_t passive_patience = 4096;
};

struct FeedStats {
  std::uint64_t datagrams = 0;
  std::uint64_t bytes = 0;
  std::uint64_t frames_ok = 0;
  std::array<std::uint64_t, 8> decode_errors{};  // indexed by DecodeStatus
  std::uint64_t invalid = 0;                     // decoded but semantically invalid
  std::uint64_t retransmit_requests = 0;
  std::uint64_t retransmit_failures = 0;
  std::uint64_t retransmitted_frames = 0;

  std::uint64_t decode_error_total() const noexcept {
    std::uint64_t n = 0;
    for (auto v : decode_errors) n += v;
    return n;
  }
};

// Feed handler: bytes -> decode -> checksum -> sequence -> validate -> sink.
//
// One call per datagram / recorded record (which may carry several frames).
// Corrupt frames are dropped at decode time; the resulting hole in the
// sequence is then indistinguishable from packet loss and goes through the
// same recovery path. That is deliberate: there is exactly one recovery
// mechanism to reason about and test.
class FeedHandler {
 public:
  using RecordHook = std::function<void(std::span<const std::byte>, std::uint64_t ingress_ns)>;

  FeedHandler(const InstrumentUniverse& universe, const FeedHandlerConfig& cfg = {},
              RetransmitSource* rewind = nullptr)
      : universe_(universe), cfg_(cfg), rewind_(rewind), seq_(cfg.reorder_window) {}

  // Called for every datagram the handler processes, including retransmitted
  // batches, before decoding. Used by the recorder.
  void set_record_hook(RecordHook hook) { record_ = std::move(hook); }

  template <class Sink>
  void on_datagram(std::span<const std::byte> bytes, std::uint64_t ingress_ns, Sink&& sink) {
    if (record_) record_(bytes, ingress_ns);
    process(bytes, ingress_ns, sink);
    recover(ingress_ns, sink);
  }

  // End of stream: resolve any open gap (fetch or abandon).
  template <class Sink>
  void finish(Sink&& sink) {
    recover(0, sink);
    if (seq_.state() == FeedState::Recovering) seq_.give_up(sink);
  }

  const FeedStats& stats() const noexcept { return stats_; }
  const Sequencer& sequencer() const noexcept { return seq_; }

 private:
  bool valid(const MarketEvent& ev) const noexcept {
    if (ev.type == EventType::Heartbeat) return true;
    if (!universe_.contains(ev.instrument)) return false;
    const InstrumentSpec& s = universe_[ev.instrument];
    switch (ev.type) {
      case EventType::AddOrder:
      case EventType::ModifyOrder:
      case EventType::Trade:
        return ev.order_id != 0 && ev.qty > 0 && s.in_band(ev.price) && s.on_tick(ev.price);
      case EventType::CancelOrder: return ev.order_id != 0;
      default: return false;
    }
  }

  template <class Sink>
  void process(std::span<const std::byte> bytes, std::uint64_t ingress_ns, Sink& sink) {
    ++stats_.datagrams;
    stats_.bytes += bytes.size();
    auto validated = [&](const MarketEvent& ev) {
      if (static_cast<std::uint8_t>(ev.type) >= static_cast<std::uint8_t>(EventType::FeedGap) || valid(ev)) {
        sink(ev);
      } else {
        ++stats_.invalid;
      }
    };
    std::size_t pos = 0;
    MarketEvent ev;
    while (pos < bytes.size()) {
      const auto r = protocol::decode(bytes.subspan(pos), ev, cfg_.verify_checksum);
      if (r.status != protocol::DecodeStatus::Ok) {
        ++stats_.decode_errors[static_cast<std::size_t>(r.status)];
        if (r.consumed == 0) break;  // cannot resynchronise within this datagram
        pos += r.consumed;
        continue;
      }
      pos += r.consumed;
      ++stats_.frames_ok;
      ev.ingress_ns = ingress_ns;
      if (seq_.state() == FeedState::Recovering) ++since_gap_;
      seq_.on_message(ev, validated);
    }
  }

  template <class Sink>
  void recover(std::uint64_t ingress_ns, Sink& sink) {
    GapRange gap;
    while (seq_.take_request(gap)) {
      if (rewind_ == nullptr) break;  // passive: wait for the frames to show up
      ++stats_.retransmit_requests;
      scratch_.clear();
      if (!rewind_->fetch(gap.first, gap.last, scratch_)) {
        ++stats_.retransmit_failures;
        seq_.give_up(sink);
        break;
      }
      stats_.retransmitted_frames += gap.last - gap.first + 1;
      std::vector<std::byte> batch;
      batch.swap(scratch_);
      if (record_) record_(batch, ingress_ns);
      process(batch, ingress_ns, sink);
    }
    if (seq_.state() != FeedState::Recovering) {
      since_gap_ = 0;
    } else if (rewind_ == nullptr && since_gap_ > cfg_.passive_patience) {
      seq_.give_up(sink);
      since_gap_ = 0;
    }
  }

  const InstrumentUniverse& universe_;
  FeedHandlerConfig cfg_;
  RetransmitSource* rewind_;
  Sequencer seq_;
  FeedStats stats_;
  RecordHook record_;
  std::vector<std::byte> scratch_;
  std::uint64_t since_gap_ = 0;
};

}  // namespace pulse::md
