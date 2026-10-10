#pragma once

#include <cstddef>
#include <new>

namespace pulse {

// 64 bytes on x86-64 and most ARM cores. Apple M-series report 128-byte
// L2 lines, but 64 is what matters for L1 false sharing; we use 64 and
// document it rather than rely on std::hardware_destructive_interference_size,
// whose value differs between compilers and triggers ABI warnings on GCC.
inline constexpr std::size_t kCacheLine = 64;

// Spin-wait hint: PAUSE on x86, YIELD on ARM. Reduces power and, on x86,
// avoids the memory-order-violation pipeline flush when the spin exits.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

// Best-effort pin of the calling thread to a CPU. Linux: sched affinity.
// macOS has no hard affinity API; we return false and the caller logs it.
bool pin_current_thread(int cpu) noexcept;

// Name the current thread (visible in perf, htop, Instruments, lldb).
void set_current_thread_name(const char* name) noexcept;

}  // namespace pulse
