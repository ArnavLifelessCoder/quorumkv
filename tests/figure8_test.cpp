#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "figure8_scenario.hpp"
#include "persistence_scenario.hpp"
#include "quorumkv/sim_cluster.hpp"

namespace quorumkv::sim {
namespace {

using figure8::Deliver;
using figure8::ElectS1AndReplicateOldTermToMajority;
using figure8::Figure8Config;
using figure8::S5Campaign;

TEST(Figure8, OldTermMajorityRemainsUncommittedAndCanBeOverwritten) {
  Cluster cluster(Figure8Config());
  ElectS1AndReplicateOldTermToMajority(cluster);
  ASSERT_EQ(cluster.CommitIndex(1), 0U);
  S5Campaign(cluster);
  ASSERT_EQ(cluster.Leaders(), (std::vector<NodeId>{5}));
  // Drain actual RPCs; a rejection produces the next fast-backup attempt.
  for (unsigned step = 0; step < 100 && !cluster.Pending().empty(); ++step) {
    cluster.Deliver(cluster.Pending().front().first);
  }
  ASSERT_GE(cluster.CommitIndex(5), 3U);
  ASSERT_GE(cluster.Committed().size(), 3U);
  EXPECT_EQ(cluster.Committed()[1].term, 3U);
  EXPECT_EQ(cluster.Log(3).entries[1].term, 3U);
  EXPECT_EQ(cluster.Stats().steps, cluster.Stats().safety_checks);
}

TEST(Figure8, CurrentTermMajorityCommitsPrefixAndPreventsStaleLeaderElection) {
  Cluster cluster(Figure8Config());
  ElectS1AndReplicateOldTermToMajority(cluster);
  ASSERT_EQ(cluster.CommitIndex(1), 0U);
  Deliver<AppendEntries>(cluster, 1, 3);
  Deliver<AppendResponse>(cluster, 3, 1);
  ASSERT_EQ(cluster.CommitIndex(1), 3U);
  ASSERT_EQ(cluster.Committed()[1].term, 2U);
  S5Campaign(cluster);
  EXPECT_TRUE(cluster.Leaders().empty());
  EXPECT_EQ(cluster.Committed()[1].term, 2U);
}

TEST(SimReplication, CommittedPrefixSurvivesAllNodeRestartAndReplaysExactly) {
  const auto run = [] {
    Cluster cluster({.seed = 42, .record_trace = true});
    cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 20, .max_delay = 3});
    for (unsigned tick = 0; tick < 100 && cluster.Leaders().empty(); ++tick) {
      cluster.Advance();
    }
    if (cluster.Leaders().size() != 1) {
      throw std::logic_error("no initial leader");
    }
    const auto index = cluster.Propose(cluster.Leaders().front(), {4, 2});
    for (unsigned tick = 0; tick < 30; ++tick) {
      cluster.Advance();
    }
    if (!index || cluster.Committed().size() < *index) {
      throw std::logic_error("proposal did not commit");
    }
    const auto committed = cluster.Committed();
    for (NodeId id = 1; id <= 3; ++id) {
      cluster.Crash(id);
    }
    for (NodeId id = 1; id <= 3; ++id) {
      cluster.Restart(id);
    }
    for (unsigned tick = 0; tick < 100; ++tick) {
      cluster.Advance();
    }
    EXPECT_TRUE(
        std::ranges::equal(committed, cluster.Committed() | std::views::take(committed.size())));
    EXPECT_EQ(cluster.Stats().log_checks, 3 * cluster.Stats().steps);
    EXPECT_EQ(cluster.Stats().application_checks, 3 * cluster.Stats().steps);
    return cluster.Trace();
  };
  EXPECT_EQ(run(), run());
}

TEST(SimReplication, FollowerAckCrashThenLeaderCrashRetainsCommittedCommand) {
  Cluster cluster({.seed = 6, .manual_delivery = true});
  CrashAfterAcknowledgement(cluster);
  ASSERT_EQ(cluster.Leaders(), (std::vector<NodeId>{2}));
  ASSERT_EQ(cluster.Log(2).entries.size(), 3U);
  EXPECT_EQ(cluster.Log(2).entries[1], cluster.Committed()[1]);
  EXPECT_EQ(cluster.Committed()[1].command, (Bytes{6}));
}

TEST(SimReplication, LongCommittedPrefixSurvivesReorderedPipelineAndLeaderReplacement) {
  Cluster cluster({.seed = 303, .size = 5, .max_append_entries = 2});
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  for (unsigned tick = 0; tick < 100 && cluster.Leaders().empty(); ++tick) {
    cluster.Advance();
  }
  ASSERT_EQ(cluster.Leaders().size(), 1U);
  const auto leader = cluster.Leaders().front();
  cluster.SetFaults({.drop_percent = 10, .duplicate_percent = 50, .max_delay = 5});
  for (unsigned command = 0; command < 80; ++command) {
    ASSERT_TRUE(cluster.Propose(leader, {static_cast<std::uint8_t>(command)}));
  }
  for (unsigned tick = 0; tick < 300 && cluster.Committed().size() < 81; ++tick) {
    cluster.Advance();
  }
  ASSERT_EQ(cluster.Committed().size(), 81U);
  const auto prefix = cluster.Committed();
  cluster.Crash(leader);
  cluster.SetFaults({.drop_percent = 15, .duplicate_percent = 30, .max_delay = 50});
  cluster.Partition({0, 1, 0, 1, 0});
  for (unsigned tick = 0; tick < 100; ++tick) {
    cluster.Advance();
  }
  cluster.Heal();
  cluster.Restart(leader);
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  for (unsigned tick = 0; tick < 200; ++tick) {
    cluster.Advance();
  }
  for (NodeId id = 1; id <= 5; ++id) {
    const auto state = cluster.Log(id);
    ASSERT_GE(cluster.CommitIndex(id), prefix.size());
    EXPECT_TRUE(std::ranges::equal(prefix, state.entries | std::views::take(prefix.size())));
  }
}

}  // namespace
}  // namespace quorumkv::sim
