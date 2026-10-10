#include "pulse/market_data/fault_injector.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace pulse::md {

bool parse_faults(const std::string& spec, FaultConfig& out, std::string& error) {
  std::stringstream ss(spec);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (item.empty()) continue;
    const auto eq = item.find('=');
    if (eq == std::string::npos) {
      error = "expected key=value, got '" + item + "'";
      return false;
    }
    const std::string key = item.substr(0, eq);
    const std::string val = item.substr(eq + 1);
    char* end = nullptr;
    const double d = std::strtod(val.c_str(), &end);
    if (end == val.c_str() || *end != '\0') {
      error = "bad number for '" + key + "': " + val;
      return false;
    }
    if (key == "drop") out.drop = d;
    else if (key == "burst") out.burst_drop = d;
    else if (key == "burst_len") out.burst_len = static_cast<std::uint32_t>(d);
    else if (key == "dup" || key == "duplicate") out.duplicate = d;
    else if (key == "reorder") out.reorder = d;
    else if (key == "corrupt") out.corrupt = d;
    else if (key == "truncate") out.truncate = d;
    else if (key == "seed") out.seed = static_cast<std::uint64_t>(d);
    else {
      error = "unknown fault '" + key + "'";
      return false;
    }
  }
  return true;
}

std::string describe(const FaultConfig& c) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "drop=%g burst=%g(len %u) dup=%g reorder=%g corrupt=%g truncate=%g seed=%llu",
                c.drop, c.burst_drop, c.burst_len, c.duplicate, c.reorder, c.corrupt, c.truncate,
                static_cast<unsigned long long>(c.seed));
  return buf;
}

void FaultInjector::deliver(std::span<const std::byte> frame, const Sink& out) {
  ++stats_.frames_out;
  out(frame);
}

void FaultInjector::process(std::span<const std::byte> frame, const Sink& out) {
  ++stats_.frames_in;

  if (burst_left_ > 0) {
    --burst_left_;
    ++stats_.dropped;
    return;
  }
  if (chance(cfg_.burst_drop)) {
    burst_left_ = cfg_.burst_len > 0 ? cfg_.burst_len - 1 : 0;
    ++stats_.dropped;
    return;
  }
  if (chance(cfg_.drop)) {
    ++stats_.dropped;
    return;
  }

  std::span<const std::byte> f = frame;
  if (chance(cfg_.corrupt)) {
    scratch_.assign(frame.begin(), frame.end());
    // Never touch the length field (bytes 0-1): a corrupted length is a
    // different failure (stream desync) exercised separately in tests.
    const auto pos = 2 + rng_() % (scratch_.size() - 2);
    scratch_[pos] ^= static_cast<std::byte>(1u << (rng_() % 8));
    f = scratch_;
    ++stats_.corrupted;
  } else if (chance(cfg_.truncate)) {
    f = frame.first(frame.size() / 2);
    ++stats_.truncated;
  }

  if (held_.empty() && chance(cfg_.reorder)) {
    held_.assign(f.begin(), f.end());
    ++stats_.reordered;
    return;
  }

  deliver(f, out);
  if (chance(cfg_.duplicate)) {
    ++stats_.duplicated;
    deliver(f, out);
  }
  if (!held_.empty()) {
    std::vector<std::byte> h;
    h.swap(held_);
    deliver(h, out);
  }
}

void FaultInjector::flush(const Sink& out) {
  if (!held_.empty()) {
    std::vector<std::byte> h;
    h.swap(held_);
    deliver(h, out);
  }
}

}  // namespace pulse::md
