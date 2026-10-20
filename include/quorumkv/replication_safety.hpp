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
  [[nodiscard]] auto committed() const -> const std::vector<LogEntry>& { return committed_; }

 private:
  struct Link {
    LogEntry entry;
    Term previous_term{};
    auto operator==(const Link&) const -> bool = default;
  };
  std::map<std::pair<LogIndex, Term>, Link> identities_;
  std::map<NodeId, LogIndex> committed_by_node_;
  std::set<std::pair<Term, NodeId>> leaders_;
  std::vector<LogEntry> committed_;
};

}  // namespace quorumkv::sim
