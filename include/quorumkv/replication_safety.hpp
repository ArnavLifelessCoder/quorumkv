#pragma once

#include <map>
#include <set>

#include "quorumkv/log.hpp"

namespace quorumkv::sim {

// An independent history oracle, never used to make protocol decisions.
// Histories survive node crashes and log truncation.
class ReplicationSafety {
 public:
  void ObserveLog(NodeId id, const RaftLog& log);
  void ObserveCommit(NodeId id, const RaftLog& log, LogIndex commit);
  void ObserveLeader(NodeId id, Term term, const RaftLog& log);
  // Deterministic state machines must produce byte-identical snapshots at one index.
  void ObserveSnapshot(const Snapshot& snapshot);
  [[nodiscard]] auto SnapshotAt(LogIndex index) const -> const Snapshot*;
  [[nodiscard]] auto committed() const -> const std::vector<LogEntry>& { return committed_; }

 private:
  void CheckSnapshotPoint(const RaftLog& log) const;
  struct Link {
    LogEntry entry;
    Term previous_term{};
    auto operator==(const Link&) const -> bool = default;
  };
  std::map<std::pair<LogIndex, Term>, Link> identities_;
  std::map<NodeId, LogIndex> committed_by_node_;
  std::set<std::pair<Term, NodeId>> leaders_;
  std::vector<LogEntry> committed_;
  std::map<LogIndex, Snapshot> snapshots_;
};

}  // namespace quorumkv::sim
