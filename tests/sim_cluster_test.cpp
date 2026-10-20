#include "quorumkv/sim_cluster.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace quorumkv::sim {
namespace {

auto WaitLeader(Cluster& cluster) -> NodeId {
  for (unsigned tick = 0; tick < 300; ++tick) {
    cluster.Advance();
    if (const auto leaders = cluster.Leaders(); leaders.size() == 1) {
      return leaders.front();
    }
  }
  throw std::runtime_error("no unique leader within 300 ticks");
}

TEST(SimCluster, ColdStartCrashAndRestartKeepDurableVotes) {
  Cluster cluster({.seed = 42});
  const auto leader = WaitLeader(cluster);
  const auto state = cluster.State(leader);
  cluster.Crash(leader);
  EXPECT_FALSE(cluster.Alive(leader));
  EXPECT_EQ(cluster.State(leader), state);
  const auto replacement = WaitLeader(cluster);
  EXPECT_NE(replacement, leader);
  EXPECT_GT(cluster.State(replacement).term, state.term);
  cluster.Restart(leader);
  EXPECT_EQ(cluster.State(leader), state);
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  for (unsigned tick = 0; tick < 100; ++tick) {
    cluster.Advance();
  }
  ASSERT_EQ(cluster.Leaders().size(), 1U);
  EXPECT_EQ(cluster.Stats().steps, cluster.Stats().safety_checks);
}

TEST(SimCluster, CompletePartitionCannotElectAndHealingRestoresLiveness) {
  Cluster cluster({.seed = 17});
  cluster.Partition({1, 2, 3});
  for (unsigned tick = 0; tick < 100; ++tick) {
    cluster.Advance();
    EXPECT_TRUE(cluster.Leaders().empty());
  }
  EXPECT_GT(cluster.Stats().dropped, 0U);
  cluster.Heal();
  EXPECT_NE(WaitLeader(cluster), 0U);
}

TEST(SimCluster, IsolatedLeaderAndMajorityLeaderHaveDifferentTermsAndHeal) {
  Cluster cluster({.seed = 91, .size = 5});
  const auto original = WaitLeader(cluster);
  std::vector<std::uint64_t> groups(5);
  groups.at(static_cast<std::size_t>(original - 1)) = 1;
  cluster.Partition(groups);
  for (unsigned tick = 0; tick < 150; ++tick) {
    cluster.Advance();
  }
  const auto leaders = cluster.Leaders();
  ASSERT_EQ(leaders.size(), 2U);
  EXPECT_NE(cluster.State(leaders.front()).term, cluster.State(leaders.back()).term);
  cluster.Heal();
  for (unsigned tick = 0; tick < 100; ++tick) {
    cluster.Advance();
  }
  EXPECT_EQ(cluster.Leaders().size(), 1U);
}

TEST(SimCluster, TraceIncludesEveryStepAndReplaysByteForByte) {
  auto exercise = [](Cluster& cluster) {
    cluster.SetFaults({.drop_percent = 20, .duplicate_percent = 50, .max_delay = 7});
    WaitLeader(cluster);
    cluster.Partition({1, 2, 2, 2, 1});
    cluster.FailNextSync(2, SyncFailure::BeforeSync);
    for (unsigned tick = 0; tick < 80; ++tick) {
      cluster.Advance();
    }
    cluster.Crash(4);
    cluster.Heal();
    cluster.Restart(4);
  };
  Cluster first({.seed = 123, .size = 5, .record_trace = true});
  Cluster second({.seed = 123, .size = 5, .record_trace = true});
  exercise(first);
  exercise(second);
  EXPECT_EQ(first.Stats(), second.Stats());
  EXPECT_EQ(first.Digest(), second.Digest());
  EXPECT_EQ(first.Trace(), second.Trace());
  EXPECT_FALSE(first.Trace().empty());
  EXPECT_GT(first.Stats().duplicated, 0U);
  EXPECT_GT(first.Stats().delivered, 0U);
  EXPECT_GT(first.Stats().dropped, 0U);
  EXPECT_EQ(first.Stats().steps, first.Stats().safety_checks);
}

TEST(SimCluster, FailedSyncStopsNodeAndRestartUsesOnlyDurableState) {
  for (const auto failure : {SyncFailure::BeforeSync, SyncFailure::AfterSync}) {
    Cluster cluster({.seed = 7});
    cluster.Partition({1, 2, 3});
    cluster.FailNextSync(1, failure);
    for (unsigned tick = 0; tick < 31; ++tick) {
      cluster.Advance();
    }
    EXPECT_FALSE(cluster.Alive(1));
    EXPECT_EQ(cluster.State(1).term, failure == SyncFailure::BeforeSync ? 0U : 1U);
    EXPECT_EQ(cluster.Stats().sync_failures, 1U);
    cluster.Restart(1);
    cluster.Heal();
    EXPECT_NE(WaitLeader(cluster), 0U);
  }
}

TEST(SimCluster, RejectsInvalidScenarioInputs) {
  EXPECT_THROW(Cluster(ClusterConfig{.size = 2}), std::invalid_argument);
  Cluster cluster({});
  EXPECT_THROW(cluster.Crash(0), std::out_of_range);
  EXPECT_THROW(cluster.Partition({1, 2}), std::invalid_argument);
  EXPECT_THROW(cluster.SetFaults(NetworkFaults{.drop_percent = 101}), std::invalid_argument);
  EXPECT_THROW(cluster.SetFaults(NetworkFaults{.max_delay = 0}), std::invalid_argument);
}

}  // namespace
}  // namespace quorumkv::sim
