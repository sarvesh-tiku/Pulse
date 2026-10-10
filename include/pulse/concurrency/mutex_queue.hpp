#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace pulse::conc {

// Bounded blocking queue: std::mutex + two condition variables over a ring.
// This is the conventional baseline. Every operation takes the lock, and a
// sleeping peer is woken through the kernel (futex / ulock) — cheap when
// uncontended, but each hand-off to a *sleeping* consumer costs a syscall and
// a context switch, and under contention both sides serialise on one line.
template <class T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity)
      : capacity_(std::max<std::size_t>(capacity, 1)), slots_(std::make_unique<T[]>(capacity_)) {}

  std::size_t capacity() const noexcept { return capacity_; }

  bool try_push(const T& v) {
    {
      std::lock_guard lk(mu_);
      if (size_ == capacity_) return false;
      slots_[(head_ + size_) % capacity_] = v;
      ++size_;
    }
    not_empty_.notify_one();
    return true;
  }

  // Blocks while full. Returns 1 if it had to wait (back-pressure), else 0.
  std::size_t push(const T& v) {
    std::size_t waited = 0;
    {
      std::unique_lock lk(mu_);
      if (size_ == capacity_) {
        waited = 1;
        not_full_.wait(lk, [&] { return size_ < capacity_; });
      }
      slots_[(head_ + size_) % capacity_] = v;
      ++size_;
    }
    not_empty_.notify_one();
    return waited;
  }

  bool try_pop(T& out) {
    {
      std::lock_guard lk(mu_);
      if (size_ == 0) return false;
      out = slots_[head_];
      head_ = (head_ + 1) % capacity_;
      --size_;
    }
    not_full_.notify_one();
    return true;
  }

  // Pops up to `max` items under a single lock acquisition.
  template <class F>
  std::size_t pop_batch(F&& f, std::size_t max) {
    std::size_t n = 0;
    {
      std::lock_guard lk(mu_);
      while (n < max && size_ > 0) {
        f(slots_[head_]);
        head_ = (head_ + 1) % capacity_;
        --size_;
        ++n;
      }
    }
    if (n) not_full_.notify_one();
    return n;
  }

  bool wait_nonempty(std::chrono::microseconds timeout) {
    std::unique_lock lk(mu_);
    return not_empty_.wait_for(lk, timeout, [&] { return size_ > 0; });
  }

  std::size_t size_approx() const {
    std::lock_guard lk(mu_);
    return size_;
  }

 private:
  const std::size_t capacity_;
  std::unique_ptr<T[]> slots_;
  mutable std::mutex mu_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

}  // namespace pulse::conc
