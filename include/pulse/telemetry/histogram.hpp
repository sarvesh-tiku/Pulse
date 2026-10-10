#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <string>

namespace pulse::telemetry {

// Log-linear latency histogram (HdrHistogram-style, fixed memory, O(1)
// record, no allocation). Values below 128 are exact; above that each
// power-of-two range is split into 64 linear sub-buckets, so the reported
// value is within ~1.6% of the true one across the full 64-bit range.
//
// Each thread owns its own histograms and they are merged after the run, so
// recording is a plain increment with no atomics or sharing.
class LatencyHistogram {
 public:
  static constexpr std::size_t kBuckets = 128 + 57 * 64;

  void record(std::uint64_t v) noexcept {
    ++counts_[index_of(v)];
    ++total_;
    sum_ += v;
    if (v < min_) min_ = v;
    if (v > max_) max_ = v;
  }

  void merge(const LatencyHistogram& o) noexcept;
  void reset() noexcept { *this = LatencyHistogram{}; }

  std::uint64_t count() const noexcept { return total_; }
  std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept { return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0; }
  // Value at percentile p in [0, 100].
  std::uint64_t percentile(double p) const noexcept;

  // "p50 1.21us  p99 3.40us  p99.9 8.01us  max 41.0us  (n=...)"
  std::string summary() const;

  static std::size_t index_of(std::uint64_t v) noexcept {
    if (v < 128) return static_cast<std::size_t>(v);
    const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v));
    const unsigned shift = msb - 6;
    const auto mantissa = static_cast<std::size_t>(v >> shift);  // [64, 128)
    return 128 + (shift - 1) * 64 + (mantissa - 64);
  }
  // Midpoint of the bucket's value range.
  static std::uint64_t value_of(std::size_t idx) noexcept {
    if (idx < 128) return idx;
    const std::size_t shift = (idx - 128) / 64 + 1;
    const std::uint64_t mantissa = (idx - 128) % 64 + 64;
    const std::uint64_t lo = mantissa << shift;
    return lo + ((std::uint64_t{1} << shift) >> 1);
  }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = ~std::uint64_t{0};
  std::uint64_t max_ = 0;
};

std::string format_ns(double ns);

}  // namespace pulse::telemetry
