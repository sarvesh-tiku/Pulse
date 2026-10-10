#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "pulse/core/cpu.hpp"

namespace pulse::conc {

// RAII thread that joins on destruction and carries its own stop flag.
// (std::jthread/std::stop_token would do this, but Apple's libc++ still ships
// them behind -fexperimental-library.)
class Worker {
 public:
  Worker() = default;
  Worker(std::string name, int cpu, std::function<void(const std::atomic<bool>& stop)> body)
      : stop_(std::make_unique<std::atomic<bool>>(false)) {
    auto* stop = stop_.get();
    thread_ = std::thread([name = std::move(name), cpu, body = std::move(body), stop] {
      set_current_thread_name(name.c_str());
      if (cpu >= 0) pin_current_thread(cpu);
      body(*stop);
    });
  }
  Worker(Worker&&) noexcept = default;
  Worker& operator=(Worker&& o) noexcept {
    join();
    thread_ = std::move(o.thread_);
    stop_ = std::move(o.stop_);
    return *this;
  }
  ~Worker() { join(); }

  void request_stop() noexcept {
    if (stop_) stop_->store(true, std::memory_order_release);
  }
  void join() {
    if (thread_.joinable()) {
      request_stop();
      thread_.join();
    }
  }
  bool joinable() const noexcept { return thread_.joinable(); }

 private:
  std::thread thread_;
  std::unique_ptr<std::atomic<bool>> stop_;
};

}  // namespace pulse::conc
