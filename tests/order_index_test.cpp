// Randomized differential test for the open-addressing id index.
//
// Backward-shift deletion is the easiest part of the engine to get subtly wrong:
// a bad shift condition loses a key, and a lost key only shows up later as a
// cancel that "fails to find" a resting order. This test drives the index with
// the same kind of churn a real feed produces and cross-checks every operation
// against std::unordered_map.
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "ultralob/order_index.hpp"

namespace {

using ultralob::kNoIndex;
using ultralob::OrderId;
using ultralob::OrderIndex;

std::uint64_t next_id(std::mt19937_64& rng) { return 1 + (rng() % 200'000); }

}  // namespace

TEST(OrderIndex, RandomizedChurnMatchesUnorderedMap) {
  std::mt19937_64 rng(0xC0FFEEull);
  OrderIndex index;
  std::unordered_map<OrderId, std::uint32_t> reference;

  constexpr int kOps = 300'000;
  for (int op = 0; op < kOps; ++op) {
    const OrderId id = next_id(rng);
    const int action = static_cast<int>(rng() % 100);
    if (action < 55) {
      const std::uint32_t slot = static_cast<std::uint32_t>(rng() % 4096);
      const bool inserted = index.insert(id, slot);
      const bool reference_inserted = reference.emplace(id, slot).second;
      ASSERT_EQ(inserted, reference_inserted) << "insert disagreement at op " << op;
      if (inserted) {
        ASSERT_EQ(index.size(), reference.size());
      }
    } else {
      const bool erased = index.erase(id);
      const bool reference_erased = reference.erase(id) > 0;
      ASSERT_EQ(erased, reference_erased) << "erase disagreement at op " << op;
      ASSERT_EQ(index.size(), reference.size());
    }

    // Full verification every 15k operations: every live key must be findable,
    // every erased key must be absent.
    if (op % 15'000 == 0) {
      for (const auto& entry : reference) {
        ASSERT_EQ(index.find(entry.first), entry.second) << "lost key " << entry.first;
      }
      ASSERT_EQ(index.size(), reference.size());
    }
  }

  for (const auto& entry : reference) {
    ASSERT_EQ(index.find(entry.first), entry.second);
  }
  ASSERT_EQ(index.size(), reference.size());
}

TEST(OrderIndex, EraseAndReinsertSameIdsKeepsProbeChainsValid) {
  OrderIndex index;
  // Insert a cluster of ids that share a hash prefix region by construction.
  std::vector<OrderId> ids;
  for (OrderId id = 1; id <= 2'000; ++id) ids.push_back(id);

  for (std::size_t i = 0; i < ids.size(); ++i) {
    ASSERT_TRUE(index.insert(ids[i], static_cast<std::uint32_t>(i)));
  }
  // Erase every other id, then verify the survivors are still reachable: this
  // is exactly the pattern backward-shift deletion must keep correct.
  for (std::size_t i = 0; i < ids.size(); i += 2) {
    ASSERT_TRUE(index.erase(ids[i]));
  }
  for (std::size_t i = 1; i < ids.size(); i += 2) {
    EXPECT_EQ(index.find(ids[i]), static_cast<std::uint32_t>(i)) << "id " << ids[i];
  }
  for (std::size_t i = 0; i < ids.size(); i += 2) {
    EXPECT_EQ(index.find(ids[i]), kNoIndex) << "id " << ids[i];
  }

  // Refill the freed half and verify the whole set again.
  for (std::size_t i = 0; i < ids.size(); i += 2) {
    ASSERT_TRUE(index.insert(ids[i], static_cast<std::uint32_t>(i)));
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    EXPECT_EQ(index.find(ids[i]), static_cast<std::uint32_t>(i)) << "id " << ids[i];
  }
  EXPECT_EQ(index.size(), ids.size());
}

TEST(OrderIndex, RejectsInvalidAndDuplicateIds) {
  OrderIndex index;
  EXPECT_FALSE(index.insert(0, 1));  // kInvalidId is never a valid key
  EXPECT_TRUE(index.insert(42, 7));
  EXPECT_FALSE(index.insert(42, 9));  // duplicate
  EXPECT_EQ(index.find(42), 7u);
  EXPECT_EQ(index.find(0), kNoIndex);
  EXPECT_FALSE(index.erase(0));
}
