#include "quorumkv/sim_support.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace quorumkv::sim {
namespace {

TEST(SimSupport, GeneratorMatchesKnownSequenceAndClockAdvancesOnlyExplicitly) {
  SeededRandom random(0);
  EXPECT_EQ(random.Next(), 0xe220a8397b1dcdafULL);
  EXPECT_EQ(random.Next(), 0x6e789e6aa1b965f4ULL);
  VirtualClock clock;
  EXPECT_EQ(clock.Now(), 0U);
  EXPECT_EQ(clock.Now(), 0U);
  clock.Advance();
  EXPECT_EQ(clock.Now(), 1U);
}

TEST(SimSupport, CrashBeforeSyncDiscardsPendingVoteButAfterSyncRetainsIt) {
  MemoryStorage storage;
  StorageBatch batch;
  batch.hard_state = HardState{.term = 1, .voted_for = 2};
  storage.FailNext(SyncFailure::BeforeSync);
  EXPECT_THROW(storage.Persist(batch), std::runtime_error);
  EXPECT_EQ(storage.Load().hard_state, (HardState{}));
  storage.Crash();
  EXPECT_EQ(storage.Load().hard_state, (HardState{}));
  storage.FailNext(SyncFailure::AfterSync);
  EXPECT_THROW(storage.Persist(batch), std::runtime_error);
  storage.Crash();
  EXPECT_EQ(storage.Load().hard_state, *batch.hard_state);
  batch.hard_state = HardState{.term = 2, .voted_for = std::nullopt};
  storage.Persist(batch);
  EXPECT_EQ(storage.Load().hard_state, *batch.hard_state);
}

TEST(SimSupport, LoadedStateIsACopyAndUnsupportedWritesAreRejected) {
  PersistentState initial;
  initial.hard_state = {.term = 2, .voted_for = 1};
  MemoryStorage storage(initial);
  auto loaded = storage.Load();
  loaded.hard_state.term = 3;
  EXPECT_EQ(storage.hard_state().term, 2U);
  StorageBatch batch;
  batch.snapshot = Snapshot{};
  EXPECT_THROW(storage.Persist(batch), std::invalid_argument);
}

TEST(SimSupport, LogTruncationAppendAndVotePublishAsOneDurableBatch) {
  PersistentState initial;
  initial.hard_state = {.term = 2, .voted_for = 1};
  initial.entries = {{.index = 1, .term = 1, .command = {}},
                     {.index = 2, .term = 2, .kind = EntryKind::Command, .command = {2}}};
  StorageBatch batch;
  batch.hard_state = HardState{.term = 3, .voted_for = 2};
  batch.truncate_from = 2;
  batch.append = {{.index = 2, .term = 3, .kind = EntryKind::Command, .command = {3}},
                  {.index = 3, .term = 3, .kind = EntryKind::Command, .command = {4}}};
  MemoryStorage storage(initial);
  storage.FailNext(SyncFailure::BeforeSync);
  EXPECT_THROW(storage.Persist(batch), std::runtime_error);
  EXPECT_EQ(storage.Load(), initial);
  storage.Crash();
  EXPECT_EQ(storage.Load(), initial);
  storage.FailNext(SyncFailure::AfterSync);
  EXPECT_THROW(storage.Persist(batch), std::runtime_error);
  storage.Crash();
  const auto durable = storage.Load();
  EXPECT_EQ(durable.hard_state, *batch.hard_state);
  ASSERT_EQ(durable.entries.size(), 3U);
  EXPECT_EQ(durable.entries.front(), initial.entries.front());
  EXPECT_EQ(durable.entries[1], batch.append.front());
  EXPECT_EQ(durable.entries.back(), batch.append.back());
  batch.truncate_from = 1;
  batch.append.clear();
  storage.Persist(batch);
  EXPECT_TRUE(storage.Load().entries.empty());
}

TEST(SimSupport, OracleRejectsNonSimultaneousLeadersAndLostVotes) {
  ElectionSafety safety;
  safety.Observe(1, Role::Leader, {.term = 4, .voted_for = 1});
  safety.Observe(1, Role::Follower, {.term = 5, .voted_for = std::nullopt});
  EXPECT_THROW(safety.Observe(2, Role::Leader, (HardState{4, 2})), std::logic_error);
  safety.Observe(3, Role::Follower, {.term = 5, .voted_for = 1});
  EXPECT_THROW(safety.Observe(3, Role::Follower, (HardState{5, 2})), std::logic_error);
  EXPECT_THROW(safety.Observe(3, Role::Follower, (HardState{5, std::nullopt})), std::logic_error);
  EXPECT_THROW(safety.Observe(1, Role::Follower, (HardState{3, std::nullopt})), std::logic_error);
}

}  // namespace
}  // namespace quorumkv::sim
