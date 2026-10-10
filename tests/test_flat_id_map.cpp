#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "pulse/core/flat_id_map.hpp"

using pulse::FlatIdMap;

TEST(FlatIdMap, BasicOperations) {
  FlatIdMap m(4);
  EXPECT_EQ(m.find(1), FlatIdMap::kNotFound);
  EXPECT_TRUE(m.insert(1, 10));
  EXPECT_FALSE(m.insert(1, 11));
  EXPECT_EQ(m.find(1), 10u);
  m.assign(1, 12);
  EXPECT_EQ(m.find(1), 12u);
  EXPECT_TRUE(m.erase(1));
  EXPECT_FALSE(m.erase(1));
  EXPECT_EQ(m.size(), 0u);
}

TEST(FlatIdMap, ZeroKeyIsNeverPresent) {
  FlatIdMap m(4);
  EXPECT_EQ(m.find(0), FlatIdMap::kNotFound);
  EXPECT_FALSE(m.insert(0, 1));
  EXPECT_FALSE(m.erase(0));
}

TEST(FlatIdMap, GrowsAndKeepsEntries) {
  FlatIdMap m(4);
  for (std::uint64_t k = 1; k <= 10'000; ++k) ASSERT_TRUE(m.insert(k, static_cast<std::uint32_t>(k * 3)));
  EXPECT_EQ(m.size(), 10'000u);
  EXPECT_LE(m.size() * 2, m.capacity());
  for (std::uint64_t k = 1; k <= 10'000; ++k) ASSERT_EQ(m.find(k), k * 3);
}

// Property: under random insert/erase churn the map agrees with
// std::unordered_map. Backward-shift deletion is the part most likely to be
// subtly wrong (wrap-around clusters), so use a small key space to force
// long collision chains and a table that stays small.
class FlatIdMapChurn : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(FlatIdMapChurn, MatchesReference) {
  std::mt19937_64 rng(GetParam());
  FlatIdMap m(8);
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  const std::uint64_t key_space = 64 + GetParam() % 512;
  for (int i = 0; i < 50'000; ++i) {
    const std::uint64_t k = 1 + rng() % key_space;
    switch (rng() % 3) {
      case 0: {
        const auto v = static_cast<std::uint32_t>(rng());
        ASSERT_EQ(m.insert(k, v), ref.emplace(k, v).second);
        break;
      }
      case 1:
        ASSERT_EQ(m.erase(k), ref.erase(k) == 1);
        break;
      default: {
        auto it = ref.find(k);
        ASSERT_EQ(m.find(k), it == ref.end() ? FlatIdMap::kNotFound : it->second);
      }
    }
    ASSERT_EQ(m.size(), ref.size());
  }
  for (const auto& [k, v] : ref) ASSERT_EQ(m.find(k), v);
}

INSTANTIATE_TEST_SUITE_P(Seeds, FlatIdMapChurn, ::testing::Range<std::uint64_t>(1, 13));
