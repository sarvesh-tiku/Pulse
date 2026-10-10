#include "pulse/telemetry/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pulse::telemetry {

void LatencyHistogram::merge(const LatencyHistogram& o) noexcept {
  for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
  total_ += o.total_;
  sum_ += o.sum_;
  min_ = std::min(min_, o.min_);
  max_ = std::max(max_, o.max_);
}

std::uint64_t LatencyHistogram::percentile(double p) const noexcept {
  if (total_ == 0) return 0;
  const auto rank = static_cast<std::uint64_t>(std::ceil(std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(total_)));
  const std::uint64_t target = std::max<std::uint64_t>(rank, 1);
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    seen += counts_[i];
    if (seen >= target) return std::clamp(value_of(i), min(), max_);
  }
  return max_;
}

std::string format_ns(double ns) {
  char buf[32];
  if (ns < 1e3) {
    std::snprintf(buf, sizeof(buf), "%.0fns", ns);
  } else if (ns < 1e6) {
    std::snprintf(buf, sizeof(buf), "%.2fus", ns / 1e3);
  } else if (ns < 1e9) {
    std::snprintf(buf, sizeof(buf), "%.2fms", ns / 1e6);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2fs", ns / 1e9);
  }
  return buf;
}

std::string LatencyHistogram::summary() const {
  if (total_ == 0) return "(no samples)";
  char buf[256];
  std::snprintf(buf, sizeof(buf), "p50 %-8s p95 %-8s p99 %-8s p99.9 %-8s max %-8s (n=%llu)",
                format_ns(static_cast<double>(percentile(50))).c_str(),
                format_ns(static_cast<double>(percentile(95))).c_str(),
                format_ns(static_cast<double>(percentile(99))).c_str(),
                format_ns(static_cast<double>(percentile(99.9))).c_str(),
                format_ns(static_cast<double>(max_)).c_str(), static_cast<unsigned long long>(total_));
  return buf;
}

}  // namespace pulse::telemetry
