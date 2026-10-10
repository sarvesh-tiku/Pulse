#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulse {

// Open-addressing hash map from a non-zero uint64 key to a uint32 value.
//
// Why not std::unordered_map: it is node-based (one heap allocation per
// insert, a pointer chase per lookup). Order ids are hit on every cancel,
// modify and trade, so the index sits on the hot path.
//
// Design:
//   * key 0 marks an empty slot (exchange order ids start at 1)
//   * linear probing: probe sequences walk adjacent memory
//   * Fibonacci hashing: sequential ids spread evenly across the table
//   * backward-shift deletion: no tombstones, so probe lengths do not degrade
//     under the add/cancel churn typical of market data
//   * max load factor 1/2, power-of-two capacity
class FlatIdMap {
 public:
  static constexpr std::uint32_t kNotFound = 0xFFFF'FFFFu;

  explicit FlatIdMap(std::size_t expected = 1024) { rehash(capacity_for(expected)); }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return slots_.size(); }

  std::uint32_t find(std::uint64_t key) const noexcept {
    if (key == 0) return kNotFound;
    std::size_t i = home(key);
    while (true) {
      const Slot& s = slots_[i];
      if (s.key == key) return s.value;
      if (s.key == 0) return kNotFound;
      i = (i + 1) & mask_;
    }
  }

  bool contains(std::uint64_t key) const noexcept { return find(key) != kNotFound; }

  // Returns false (and leaves the map unchanged) if the key already exists.
  bool insert(std::uint64_t key, std::uint32_t value) {
    if (key == 0) return false;
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    std::size_t i = home(key);
    while (slots_[i].key != 0) {
      if (slots_[i].key == key) return false;
      i = (i + 1) & mask_;
    }
    slots_[i] = {key, value};
    ++size_;
    return true;
  }

  void assign(std::uint64_t key, std::uint32_t value) noexcept {
    std::size_t i = home(key);
    while (slots_[i].key != key) i = (i + 1) & mask_;
    slots_[i].value = value;
  }

  bool erase(std::uint64_t key) noexcept {
    if (key == 0) return false;
    std::size_t i = home(key);
    while (slots_[i].key != key) {
      if (slots_[i].key == 0) return false;
      i = (i + 1) & mask_;
    }
    // Backward-shift: pull later entries of the same cluster into the hole
    // if doing so moves them no further from their home slot.
    std::size_t hole = i;
    std::size_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].key == 0) break;
      const std::size_t h = home(slots_[j].key);
      // Entry at j may move to `hole` iff hole lies cyclically in [h, j).
      const bool movable = ((j - h) & mask_) >= ((j - hole) & mask_);
      if (movable) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole] = {};
    --size_;
    return true;
  }

  void clear() noexcept {
    for (auto& s : slots_) s = {};
    size_ = 0;
  }

  template <class F>
  void for_each(F&& f) const {
    for (const auto& s : slots_)
      if (s.key != 0) f(s.key, s.value);
  }

 private:
  struct Slot {
    std::uint64_t key = 0;
    std::uint32_t value = 0;
  };

  static std::size_t capacity_for(std::size_t expected) {
    return std::bit_ceil(std::max<std::size_t>(16, expected * 2));
  }

  std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  void rehash(std::size_t new_cap) {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(new_cap, Slot{});
    mask_ = new_cap - 1;
    shift_ = 64 - static_cast<unsigned>(std::countr_zero(new_cap));
    size_ = 0;
    for (const auto& s : old)
      if (s.key != 0) insert(s.key, s.value);
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  unsigned shift_ = 64;
  std::size_t size_ = 0;
};

}  // namespace pulse
