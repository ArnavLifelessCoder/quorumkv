#pragma once

#include <algorithm>
#include <stdexcept>

#include "quorumkv/sim_cluster.hpp"

namespace quorumkv::sim::figure8 {

// Figure 8(a-b): S1/S2 have the term-2 suffix, S5 has the term-3 suffix.
// No response is fabricated: subsequent elections and replication use queued RPCs.
inline auto Figure8Config(std::uint64_t seed = 8) -> ClusterConfig {
  ClusterConfig config{.seed = seed,
                       .size = 5,
                       .record_trace = true,
                       .manual_delivery = true,
                       .max_append_entries = 1};
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

inline void Campaign(Cluster& cluster, NodeId id, Term term) {
  for (unsigned tick = 0; tick < 100 && cluster.State(id).term < term; ++tick) {
    cluster.Tick(id);
  }
  if (cluster.State(id).term != term) {
    throw std::runtime_error("Figure 8 election timeout");
  }
}

inline void ElectS1AndReplicateOldTermToMajority(Cluster& cluster) {
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  Campaign(cluster, 1, 4);
  for (const auto peer : {NodeId{2}, NodeId{3}}) {
    Deliver<RequestVote>(cluster, 1, peer);
    Deliver<VoteResponse>(cluster, peer, 1);
  }
  if (cluster.Leaders() != std::vector<NodeId>{1}) {
    throw std::runtime_error("Figure 8 leader election failed");
  }
  // S2 accepts the current-term no-op, but two copies are not a five-node majority.
  Deliver<AppendEntries>(cluster, 1, 2);
  Deliver<AppendResponse>(cluster, 2, 1);
  // S3 rejects prev=(2,2), then receives only index 2 because batch size is one.
  Deliver<AppendEntries>(cluster, 1, 3);
  Deliver<AppendResponse>(cluster, 3, 1);
  Deliver<AppendEntries>(cluster, 1, 3);
  Deliver<AppendResponse>(cluster, 3, 1);
}

inline void S5Campaign(Cluster& cluster) {
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

}  // namespace quorumkv::sim::figure8
