#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "pulse/core/cpu.hpp"

namespace pulse::conc {

// Single-writer sequence lock for publishing snapshots (positions, P&L, feed
// health) from the hot threads to the telemetry thread.
//
// The writer never blocks and never waits for readers — telemetry can never
// stall trading. Readers retry if they observe a write in progress.
//
// The payload is stored as relaxed atomic 64-bit words rather than a plain T:
// a plain racy memcpy is a data race (UB, and ThreadSanitizer reports it),
// whereas relaxed atomics are well-defined and compile to plain loads/stores.
template <class T>
class SeqLock {
  static_assert(std::is_trivially_copyable_v<T>);
  static constexpr std::size_t kWords = (sizeof(T) + 7) / 8;

 public:
  void store(const T& v) noexcept {
    std::array<std::uint64_t, kWords> tmp{};
    std::memcpy(tmp.data(), static_cast<const void*>(&v), sizeof(T));
    const auto s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < kWords; ++i) words_[i].store(tmp[i], std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);  // even: stable
  }

  T load() const noexcept {
    std::array<std::uint64_t, kWords> tmp{};
    while (true) {
      const auto s1 = seq_.load(std::memory_order_acquire);
      if (s1 & 1) {
        cpu_relax();
        continue;
      }
      for (std::size_t i = 0; i < kWords; ++i) tmp[i] = words_[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) == s1) break;
    }
    T v;
    // T is trivially copyable (static_assert above); the void* cast tells
    // GCC's -Wclass-memaccess that the byte copy is intended.
    std::memcpy(static_cast<void*>(&v), tmp.data(), sizeof(T));
    return v;
  }

  std::uint64_t version() const noexcept { return seq_.load(std::memory_order_acquire) / 2; }

 private:
  alignas(kCacheLine) std::atomic<std::uint64_t> seq_{0};
  std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

}  // namespace pulse::conc
