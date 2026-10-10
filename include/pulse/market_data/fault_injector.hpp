#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "pulse/core/random.hpp"

namespace pulse::md {

// Probabilities are per frame. Everything is driven by a seeded PRNG so a
// faulty session is reproducible bit-for-bit.
struct FaultConfig {
  double drop = 0.0;        // frame lost (single)
  double burst_drop = 0.0;  // start of a burst loss of `burst_len` frames
  std::uint32_t burst_len = 32;
  double duplicate = 0.0;   // frame delivered twice
  double reorder = 0.0;     // frame held back and delivered after the next one
  double corrupt = 0.0;     // one body/header byte flipped (checksum catches it)
  double truncate = 0.0;    // frame cut short (datagram truncated)
  std::uint64_t seed = 7;

  bool any() const noexcept {
    return drop > 0 || burst_drop > 0 || duplicate > 0 || reorder > 0 || corrupt > 0 || truncate > 0;
  }
};

// Parses "drop=1e-4,dup=1e-4,reorder=1e-4,corrupt=1e-5,burst=1e-6,burst_len=64,truncate=0,seed=7".
bool parse_faults(const std::string& spec, FaultConfig& out, std::string& error);
std::string describe(const FaultConfig& cfg);

struct FaultStats {
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t dropped = 0;
  std::uint64_t duplicated = 0;
  std::uint64_t reordered = 0;
  std::uint64_t corrupted = 0;
  std::uint64_t truncated = 0;
};

class FaultInjector {
 public:
  using Sink = std::function<void(std::span<const std::byte>)>;

  explicit FaultInjector(const FaultConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {}

  void process(std::span<const std::byte> frame, const Sink& out);
  // Releases a frame still held for reordering.
  void flush(const Sink& out);
  const FaultStats& stats() const noexcept { return stats_; }

 private:
  bool chance(double p) { return bernoulli(rng_, p); }
  void deliver(std::span<const std::byte> frame, const Sink& out);

  FaultConfig cfg_;
  std::mt19937_64 rng_;
  FaultStats stats_;
  std::uint32_t burst_left_ = 0;
  std::vector<std::byte> held_;
  std::vector<std::byte> scratch_;
};

}  // namespace pulse::md
