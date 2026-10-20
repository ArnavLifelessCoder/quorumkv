#include "quorumkv/replication_safety.hpp"

#include <algorithm>
#include <stdexcept>

namespace quorumkv::sim {

void ReplicationSafety::ObserveLog(NodeId id, const RaftLog& log) {
  if (log.base_index() != 0) {
    throw std::invalid_argument("replication oracle requires the full pre-snapshot log");
  }
  for (const auto& entry : log.entries()) {
    const Link link{.entry = entry, .previous_term = log.term(entry.index - 1)};
    const auto [found, inserted] = identities_.emplace(std::pair{entry.index, entry.term}, link);
    // Equal identity implies equal entry and equal predecessor identity. Induction
    // proves exact prefix equality, without relying on a hash collision assumption.
    if (!inserted && found->second != link) {
      throw std::logic_error("Log Matching: identical index/term has a different entry or prefix");
    }
  }
  const auto retained = committed_by_node_[id];
  if (log.last_index() < retained) {
    throw std::logic_error("committed entries lost after crash or truncation");
  }
  for (LogIndex index = 1; index <= retained; ++index) {
    if (log.at(index) != committed_.at(static_cast<std::size_t>(index - 1))) {
      throw std::logic_error("committed entries reordered or replaced");
    }
  }
}

void ReplicationSafety::ObserveCommit(NodeId id, const RaftLog& log, LogIndex commit) {
  if (commit > log.last_index()) {
    throw std::logic_error("commit index exceeds durable log");
  }
  // Replay from index one is the deliberately simple reference state machine.
  // It also checks prefix order when a restarted node relearns its commit index.
  for (LogIndex index = 1; index <= commit; ++index) {
    const auto& entry = log.at(index);
    if (index > committed_.size()) {
      committed_.push_back(entry);
    } else if (committed_.at(static_cast<std::size_t>(index - 1)) != entry) {
      throw std::logic_error("State Machine Safety: different entries applied at the same index");
    }
  }
  committed_by_node_[id] = std::max(committed_by_node_[id], commit);
}

void ReplicationSafety::ObserveLeader(NodeId id, Term term, const RaftLog& log) {
  if (!leaders_.emplace(term, id).second) {
    return;
  }
  if (log.last_index() < committed_.size()) {
    throw std::logic_error("Leader Completeness: elected leader lacks committed entries");
  }
  for (const auto& entry : committed_) {
    if (log.at(entry.index) != entry) {
      throw std::logic_error("Leader Completeness: elected leader changed committed prefix");
    }
  }
}

}  // namespace quorumkv::sim
