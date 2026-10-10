#pragma once

#include <cmath>
#include <cstdint>
#include <random>

namespace pulse {

// Portable random sampling.
//
// std::mt19937_64 is fully specified by the standard, but the *distributions*
// (uniform_real, exponential, geometric, ...) are implementation-defined:
// libstdc++ and libc++ turn the same engine output into different numbers.
// A session generated from seed 42 on macOS would then differ from one
// generated on Linux. Everything below uses only integer operations and exact
// IEEE arithmetic (+, *, /, frexp), and the library is compiled with
// -ffp-contract=off, so results are bit-identical across compilers.
using Rng = std::mt19937_64;

// 53 random bits -> double in [0, 1).
inline double uniform01(Rng& r) noexcept { return static_cast<double>(r() >> 11) * 0x1.0p-53; }

// True with probability p (exact integer threshold comparison).
inline bool bernoulli(Rng& r, double p) noexcept {
  if (p <= 0.0) return false;
  if (p >= 1.0) return true;
  const auto threshold = static_cast<std::uint64_t>(p * 0x1.0p53);
  return (r() >> 11) < threshold;
}

// Number of failures before the first success, success probability p.
inline std::uint32_t geometric(Rng& r, double p) noexcept {
  std::uint32_t k = 0;
  while (!bernoulli(r, p) && k < 1'000'000) ++k;
  return k;
}

// Natural log built from exact operations only (frexp + a fixed polynomial
// series), so it cannot differ between libm implementations.
inline double portable_log(double x) noexcept {
  int e = 0;
  double m = std::frexp(x, &e);  // x = m * 2^e, m in [0.5, 1)
  if (m < 0.70710678118654752) {
    m *= 2.0;
    --e;
  }
  // log(m) = 2 atanh(t), t = (m-1)/(m+1), |t| <= 0.1716
  const double t = (m - 1.0) / (m + 1.0);
  const double t2 = t * t;
  double s = 1.0 / 19.0;
  for (int k = 17; k >= 1; k -= 2) s = 1.0 / k + t2 * s;
  return 2.0 * t * s + static_cast<double>(e) * 0.69314718055994530942;
}

// Exponential with the given mean.
inline double exponential(Rng& r, double mean) noexcept {
  const double u = 1.0 - uniform01(r);  // (0, 1]
  return -portable_log(u) * mean;
}

}  // namespace pulse
