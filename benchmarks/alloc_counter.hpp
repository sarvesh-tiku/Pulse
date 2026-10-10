#pragma once

// Counts heap allocations by replacing global operator new/delete. Include in
// exactly one translation unit of a benchmark binary. Used to report
// "allocations per event" -- the metric that separates the node-based book
// from the pooled one.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace pulse::bench {
inline std::atomic<std::uint64_t> g_allocs{0};
inline std::uint64_t allocations() noexcept { return g_allocs.load(std::memory_order_relaxed); }
}  // namespace pulse::bench

void* operator new(std::size_t n) {
  pulse::bench::g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  pulse::bench::g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a) {
  pulse::bench::g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<std::size_t>(a), n ? n : 1) != 0) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
