#include "pulse/core/cpu.hpp"

#include <pthread.h>

#if defined(__linux__)
#include <sched.h>
#endif

namespace pulse {

bool pin_current_thread(int cpu) noexcept {
#if defined(__linux__)
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

void set_current_thread_name(const char* name) noexcept {
#if defined(__APPLE__)
  pthread_setname_np(name);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

}  // namespace pulse
