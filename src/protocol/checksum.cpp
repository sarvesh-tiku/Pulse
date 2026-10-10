#include "pulse/protocol/checksum.hpp"

#include <array>
#include <cstring>

#if defined(__aarch64__)
#include <arm_acle.h>
#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#endif
#define PULSE_CRC_TARGET __attribute__((target("arch=armv8-a+crc")))
#elif defined(__x86_64__)
#include <nmmintrin.h>
#define PULSE_CRC_TARGET __attribute__((target("sse4.2")))
#endif

namespace pulse::protocol {
namespace {

constexpr std::uint32_t kPoly = 0x82F63B78u;  // reflected Castagnoli polynomial

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ kPoly : c >> 1;
    t[i] = c;
  }
  return t;
}

constexpr auto kTable = make_table();

#if defined(PULSE_CRC_TARGET)
// Compiled for the CRC instruction regardless of the global -march, and only
// called after a runtime CPU check. (A plain `#if __ARM_FEATURE_CRC32` check
// silently fell back to the table on default Linux/arm64 builds, where the
// profile showed CRC at 17% of runtime.)
PULSE_CRC_TARGET std::uint32_t crc32c_hw(std::uint32_t crc, std::span<const std::byte> data) noexcept {
  crc = ~crc;
  const std::byte* p = data.data();
  std::size_t n = data.size();
  while (n >= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
#if defined(__aarch64__)
    crc = __crc32cd(crc, v);
#else
    crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, v));
#endif
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    const auto b = static_cast<std::uint8_t>(*p);
#if defined(__aarch64__)
    crc = __crc32cb(crc, b);
#else
    crc = _mm_crc32_u8(crc, b);
#endif
    ++p;
    --n;
  }
  return ~crc;
}
#endif

bool detect_hw() noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
  return true;  // every Apple arm64 CPU implements the CRC32 extension
#elif defined(__aarch64__) && defined(__linux__)
  return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#elif defined(__x86_64__)
  return __builtin_cpu_supports("sse4.2");
#else
  return false;
#endif
}

const bool kHaveHw = detect_hw();

}  // namespace

std::uint32_t crc32c_software(std::uint32_t crc, std::span<const std::byte> data) noexcept {
  crc = ~crc;
  for (std::byte b : data) {
    crc = kTable[(crc ^ static_cast<std::uint32_t>(b)) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

bool crc32c_hardware_available() noexcept { return kHaveHw; }

std::uint32_t crc32c(std::uint32_t crc, std::span<const std::byte> data) noexcept {
#if defined(PULSE_CRC_TARGET)
  if (kHaveHw) return crc32c_hw(crc, data);
#endif
  return crc32c_software(crc, data);
}

}  // namespace pulse::protocol
