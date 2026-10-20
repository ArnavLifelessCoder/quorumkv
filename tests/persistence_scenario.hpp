#pragma once

#include "figure8_scenario.hpp"

namespace quorumkv::sim {

inline void CrashAfterAcknowledgement(Cluster& cluster) {
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  figure8::Campaign(cluster, 1, 1);
  figure8::Deliver<RequestVote>(cluster, 1, 2);
  figure8::Deliver<VoteResponse>(cluster, 2, 1);
  figure8::Deliver<AppendEntries>(cluster, 1, 2);
  figure8::Deliver<AppendResponse>(cluster, 2, 1);
  if (cluster.Propose(1, {6}) != 2) {
    throw std::runtime_error("persistence schedule could not propose");
  }
  cluster.SetReplicationCrashPercent(0, 100);
  // Choose the queued command batch, which may follow a heartbeat in the queue.
  bool delivered = false;
  for (const auto& [packet, message] : cluster.Pending()) {
    const auto* append = std::get_if<AppendEntries>(&message.body);
    if (message.from == 1 && message.to == 2 && append && !append->entries.empty() &&
        append->entries.back().index == 2) {
      cluster.Deliver(packet);
      delivered = true;
      break;
    }
  }
  if (!delivered || cluster.Alive(2)) {
    throw std::runtime_error("persistence schedule missed the follower crash boundary");
  }
  figure8::Deliver<AppendResponse>(cluster, 2, 1);
  if (cluster.CommitIndex(1) != 2) {
    throw std::runtime_error("persistence schedule did not commit the acknowledged command");
  }
  cluster.Crash(1);
  cluster.SetReplicationCrashPercent(0, 0);
  cluster.Restart(2);
  figure8::Campaign(cluster, 2, 2);
  figure8::Deliver<RequestVote>(cluster, 2, 3);
  // If the acknowledgement preceded persistence, this election loses a committed
  // command and the historical oracle rejects the new leader at this step.
  figure8::Deliver<VoteResponse>(cluster, 3, 2);
}

}  // namespace quorumkv::sim
