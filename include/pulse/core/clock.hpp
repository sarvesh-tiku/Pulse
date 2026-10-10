#pragma once

#include <chrono>
#include <cstdint>

namespace pulse {

// Monotonic wall-clock nanoseconds used for *measurement* (latency stamps).
// Business logic never reads this clock: the trading core runs on market-data
// timestamps so that replays are deterministic.
//
// Resolution note: on Apple Silicon steady_clock ticks at 24 MHz (~41.7 ns),
// on x86-64 Linux CLOCK_MONOTONIC is typically ~20-30 ns per read via vDSO.
inline std::uint64_t now_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace pulse
