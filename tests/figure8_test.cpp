#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "quorumkv/sim_cluster.hpp"

namespace quorumkv::sim {
namespace {

// Figure 8(a-b): S1/S2 have the term-2 suffix, S5 has the term-3 suffix.
// No response is fabricated: subsequent elections and replication use queued RPCs.
auto Figure8Config() -> ClusterConfig {
  ClusterConfig config{
      .seed = 8, .size = 5, .record_trace = true, .manual_delivery = true, .max_append_entries = 1};
  config.initial.resize(5);
  for (auto& state : config.initial) {
    state.hard_state = {.term = 3, .voted_for = std::nullopt};
    state.entries = {{.index = 1, .term = 1, .command = {}}};
  }
  for (const auto index : {0U, 1U, 4U}) {
    config.initial[index].entries.push_back(
        {.index = 2,
         .term = index == 4 ? Term{3} : Term{2},
         .kind = EntryKind::Command,
         .command = {static_cast<std::uint8_t>(index == 4 ? 3 : 2)}});
  }
  return config;
}

template <class Body>
void Deliver(Cluster& cluster, NodeId from, NodeId to) {
  const auto pending = cluster.Pending();
  const auto found = std::ranges::find_if(pending, [&](const auto& packet) {
    return packet.second.from == from && packet.second.to == to &&
           std::holds_alternative<Body>(packet.second.body);
  });
  if (found == pending.end()) {
    throw std::logic_error("Figure 8 schedule has no matching queued packet");
  }
  cluster.Deliver(found->first);
}

void Campaign(Cluster& cluster, NodeId id, Term term) {
  for (unsigned tick = 0; tick < 100 && cluster.State(id).term < term; ++tick) {
    cluster.Tick(id);
  }
  ASSERT_EQ(cluster.State(id).term, term);
}

void ElectS1AndReplicateOldTermToMajority(Cluster& cluster) {
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  Campaign(cluster, 1, 4);
  for (const auto peer : {NodeId{2}, NodeId{3}}) {
    Deliver<RequestVote>(cluster, 1, peer);
    Deliver<VoteResponse>(cluster, peer, 1);
  }
  ASSERT_EQ(cluster.Leaders(), (std::vector<NodeId>{1}));
  // S2 accepts the current-term no-op, but two copies are not a five-node majority.
  Deliver<AppendEntries>(cluster, 1, 2);
  Deliver<AppendResponse>(cluster, 2, 1);
  // S3 rejects prev=(2,2), then receives only index 2 because batch size is one.
  Deliver<AppendEntries>(cluster, 1, 3);
  Deliver<AppendResponse>(cluster, 3, 1);
  Deliver<AppendEntries>(cluster, 1, 3);
  Deliver<AppendResponse>(cluster, 3, 1);
  ASSERT_EQ(cluster.Log(3).entries.back().term, 2U);
  ASSERT_EQ(cluster.CommitIndex(1), 0U);
}

void S5Campaign(Cluster& cluster) {
  cluster.Crash(1);
  // First attempt is term 4 and is denied by S2/S3, which already voted for S1.
  Campaign(cluster, 5, 4);
  for (const auto peer : {NodeId{2}, NodeId{3}, NodeId{4}}) {
    Deliver<RequestVote>(cluster, 5, peer);
    Deliver<VoteResponse>(cluster, peer, 5);
  }
  Campaign(cluster, 5, 5);
  for (const auto peer : {NodeId{2}, NodeId{3}, NodeId{4}}) {
    Deliver<RequestVote>(cluster, 5, peer);
    Deliver<VoteResponse>(cluster, peer, 5);
  }
}

TEST(Figure8, OldTermMajorityRemainsUncommittedAndCanBeOverwritten) {
  Cluster cluster(Figure8Config());
  ElectS1AndReplicateOldTermToMajority(cluster);
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

}  // namespace
}  // namespace quorumkv::sim
