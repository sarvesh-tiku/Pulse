#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "pulse/core/instrument.hpp"
#include "pulse/engine/trading_core.hpp"

namespace pulse::telemetry {

// Counters written by the hot threads (relaxed, each written by exactly one
// thread, updated in batches) and read by the dashboard thread.
struct LiveCounters {
  alignas(64) std::atomic<std::uint64_t> datagrams{0};
  std::atomic<std::uint64_t> gaps{0};
  std::atomic<std::uint8_t> feed_state{0};
  alignas(64) std::atomic<std::uint64_t> events{0};  // book thread
  alignas(64) std::atomic<std::uint64_t> updates{0};  // core thread
  std::atomic<std::uint64_t> p99_ns{0};
  std::atomic<std::uint64_t> p50_ns{0};
};

struct DashboardView {
  double elapsed_s = 0;
  double events_per_sec = 0;
  std::uint64_t events = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint8_t feed_state = 0;  // md::FeedState
  std::uint64_t gaps = 0;
  double q1_util = 0;  // 0..1
  double q2_util = 0;
};

std::string render_dashboard(const InstrumentUniverse& uni, const engine::CoreSnapshot& core,
                             const DashboardView& view);

}  // namespace pulse::telemetry
