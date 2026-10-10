#pragma once

#include <cstdint>
#include <string>

namespace pulse {

// Order-sensitive 64-bit running hash. Every observable decision the
// system makes (published book states, risk verdicts, execution reports,
// fills, final positions) is folded in; two runs over the same input must
// produce the same digest. That single number is how determinism is tested:
// inline vs threaded pipeline, mutex vs SPSC queues, map vs array book,
// live run vs replay of its recording.
//
// Performance note: the first version was byte-wise FNV-1a -- 8 *dependent*
// multiplies per word, ~50 per published book update -- and profiling showed
// it costing more than the order book itself. Now each word is scrambled with
// a splitmix64 finalizer (independent of the running state, so it pipelines)
// and folded in with a single dependent multiply.
class Digest {
 public:
  void mix(std::uint64_t v) noexcept {
    v ^= v >> 30;
    v *= 0xBF58476D1CE4E5B9ull;
    v ^= v >> 27;
    v *= 0x94D049BB133111EBull;
    v ^= v >> 31;
    h_ = (h_ ^ v) * 0x100000001B3ull;
    h_ ^= h_ >> 29;
  }
  void mix_signed(std::int64_t v) noexcept { mix(static_cast<std::uint64_t>(v)); }
  std::uint64_t value() const noexcept { return h_; }
  std::string hex() const;

 private:
  std::uint64_t h_ = 0xCBF29CE484222325ull;
};

}  // namespace pulse
