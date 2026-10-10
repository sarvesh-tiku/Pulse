#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include "pulse/core/cpu.hpp"

namespace pulse::conc {

// Bounded single-producer / single-consumer ring buffer. Lock-free and
// wait-free for both sides (every operation finishes in a bounded number of
// steps; there are no CAS loops).
//
// Memory ordering:
//   producer: write slot, then tail.store(release)
//   consumer: tail.load(acquire), read slot, then head.store(release)
//   producer: head.load(acquire) before reusing a slot
// The release/acquire pairs are what make the slot contents visible; no
// seq_cst fences are needed with exactly one producer and one consumer.
//
// Indices grow monotonically (size_t never wraps in practice) and are masked
// on access; full is `tail - head == capacity`, so every slot is usable.
//
// Template knobs exist so the benchmarks can measure each optimisation:
//   Padded       head and tail on separate cache lines (avoids false
//                sharing: otherwise every push invalidates the consumer's
//                line and every pop the producer's).
//   CachedIndex  each side keeps a private copy of the other side's index and
//                only re-reads the shared atomic when the copy says the ring
//                is full / empty. In steady state this removes almost all
//                cross-core cache-line traffic on the index.
template <class T, bool Padded = true, bool CachedIndex = true>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "ring slots are copied with plain assignment");
  static constexpr std::size_t kAlign = Padded ? kCacheLine : alignof(std::size_t);

 public:
  explicit SpscRing(std::size_t capacity)
      : capacity_(std::bit_ceil(std::max<std::size_t>(capacity, 2))),
        mask_(capacity_ - 1),
        slots_(std::make_unique<T[]>(capacity_)) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  std::size_t capacity() const noexcept { return capacity_; }

  // --- producer ---------------------------------------------------------
  bool try_push(const T& v) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (!has_space(t)) return false;
    slots_[t & mask_] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Spins (then yields) while full. Returns the number of failed attempts,
  // which the pipeline reports as back-pressure.
  std::size_t push(const T& v) noexcept {
    std::size_t spins = 0;
    while (!try_push(v)) {
      if (++spins < 1024) {
        cpu_relax();
      } else {
        std::this_thread::yield();
      }
    }
    return spins;
  }

  // --- consumer ---------------------------------------------------------
  bool try_pop(T& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (available(h) == 0) return false;
    out = slots_[h & mask_];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Pops up to `max` items, calling f(const T&) for each, and publishes the
  // new head once. Batching amortises the release store and the cache-line
  // hand-off of `head` over many items.
  template <class F>
  std::size_t pop_batch(F&& f, std::size_t max) noexcept(noexcept(f(std::declval<const T&>()))) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    std::size_t n = available(h);
    if (n == 0) return 0;
    if (n > max) n = max;
    for (std::size_t i = 0; i < n; ++i) f(slots_[(h + i) & mask_]);
    head_.store(h + n, std::memory_order_release);
    return n;
  }

  // Waits (spin, then yield) until data is available or `timeout` passes.
  bool wait_nonempty(std::chrono::microseconds timeout) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t spins = 0;
    while (available(head_.load(std::memory_order_relaxed)) == 0) {
      if (++spins < 256) {
        cpu_relax();
      } else {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
      }
    }
    return true;
  }

  // Approximate: exact only when called from one of the two owning threads.
  std::size_t size_approx() const noexcept {
    const std::size_t t = tail_.load(std::memory_order_acquire);
    const std::size_t h = head_.load(std::memory_order_acquire);
    return t - h;
  }

 private:
  bool has_space(std::size_t t) noexcept {
    if constexpr (CachedIndex) {
      if (t - cached_head_ < capacity_) return true;
      cached_head_ = head_.load(std::memory_order_acquire);
      return t - cached_head_ < capacity_;
    } else {
      return t - head_.load(std::memory_order_acquire) < capacity_;
    }
  }

  std::size_t available(std::size_t h) noexcept {
    if constexpr (CachedIndex) {
      if (cached_tail_ != h) return cached_tail_ - h;
      cached_tail_ = tail_.load(std::memory_order_acquire);
      return cached_tail_ - h;
    } else {
      return tail_.load(std::memory_order_acquire) - h;
    }
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;

  // Consumer-owned line: head (written by consumer) + consumer's cache of tail.
  alignas(kAlign) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;
  // Producer-owned line: tail (written by producer) + producer's cache of head.
  alignas(kAlign) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;
  // Keep whatever follows this object off the producer's line.
  alignas(kAlign) char pad_end_[1] = {};
};

}  // namespace pulse::conc
