#include "quorumkv/replication_safety.hpp"

#include <algorithm>
#include <stdexcept>

namespace quorumkv::sim {

void ReplicationSafety::CheckSnapshotPoint(const RaftLog& log) const {
  const auto base = log.base_index();
  if (base == 0) {
    return;
  }
  // A snapshot may only cover entries some node was observed to commit.
  if (base > committed_.size()) {
    throw std::logic_error("snapshot covers entries that were never committed");
  }
  if (committed_.at(static_cast<std::size_t>(base - 1)).term != log.term(base)) {
    throw std::logic_error("snapshot point disagrees with the committed history");
  }
}

void ReplicationSafety::ObserveLog(NodeId id, const RaftLog& log) {
  CheckSnapshotPoint(log);
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
  for (LogIndex index = log.base_index() + 1; index <= retained; ++index) {
    if (log.at(index) != committed_.at(static_cast<std::size_t>(index - 1))) {
      throw std::logic_error("committed entries reordered or replaced");
    }
  }
}

void ReplicationSafety::ObserveCommit(NodeId id, const RaftLog& log, LogIndex commit) {
  if (commit > log.last_index()) {
    throw std::logic_error("commit index exceeds durable log");
  }
  CheckSnapshotPoint(log);
  // Replay from the snapshot point is the deliberately simple reference state machine.
  // Entries inside the snapshot were checked when some node first committed them.
  for (LogIndex index = log.base_index() + 1; index <= commit; ++index) {
    const auto& entry = log.at(index);
    if (index > committed_.size() + 1) {
      throw std::logic_error("commit skipped entries never observed committed");
    }
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
  CheckSnapshotPoint(log);
  for (const auto& entry : committed_) {
    if (entry.index > log.base_index() && log.at(entry.index) != entry) {
      throw std::logic_error("Leader Completeness: elected leader changed committed prefix");
    }
  }
}

void ReplicationSafety::ObserveSnapshot(const Snapshot& snapshot) {
  const auto [found, inserted] = snapshots_.emplace(snapshot.last_included_index, snapshot);
  if (!inserted && found->second != snapshot) {
    throw std::logic_error("snapshots of the same index differ between replicas");
  }
}

auto ReplicationSafety::SnapshotAt(LogIndex index) const -> const Snapshot* {
  const auto found = snapshots_.find(index);
  return found == snapshots_.end() ? nullptr : &found->second;
}

}  // namespace quorumkv::sim
