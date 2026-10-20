#pragma once

#include <map>
#include <set>

#include "quorumkv/interfaces.hpp"
#include "quorumkv/log.hpp"

namespace quorumkv {

enum class Role : std::uint8_t { Follower, Candidate, Leader };

struct ElectionConfig {
  NodeId id{};
  std::vector<NodeId> members;
  TimePoint election_min{15};
  TimePoint election_max{30};
  TimePoint heartbeat_interval{3};
  std::size_t max_append_entries{16};
};

// Callers serialize Step/Tick/Propose and stop on exceptions.
class RaftNode {
 public:
  RaftNode(ElectionConfig config, Transport& transport, Clock& clock, Storage& storage,
           Random& random);
  void Step(const Message& message);
  void Tick();
  auto Propose(Bytes command) -> std::optional<LogIndex>;
  [[nodiscard]] auto role() const -> Role { return role_; }
  [[nodiscard]] auto hard_state() const -> HardState { return hard_state_; }
  [[nodiscard]] auto leader_hint() const -> std::optional<NodeId> { return leader_; }
  [[nodiscard]] auto stopped() const -> bool { return stopped_; }
  [[nodiscard]] auto log() const -> const RaftLog& { return log_; }
  [[nodiscard]] auto commit_index() const -> LogIndex { return commit_index_; }

 private:
  void Save(HardState state);
  void Persist(const StorageBatch& batch);
  auto AppendLocal(EntryKind kind, Bytes command) -> LogIndex;
  void SendAppend(NodeId peer);
  auto AdvanceCommit() -> bool;
  void ResetElectionTimer();
  void StartElection();
  void BecomeLeader();
  void Heartbeat();
  void Send(NodeId to, MessageBody body, std::uint64_t rpc_id = 0);
  void Receive(const Message& message, const RequestVote& vote);
  void Receive(const Message& message, const VoteResponse& vote);
  void Receive(const Message& message, const AppendEntries& append);
  void Receive(const Message& message, const AppendResponse& reply);
  void EnsureRunning() const;

  ElectionConfig config_;
  Transport& transport_;
  Clock& clock_;
  Storage& storage_;
  Random& random_;
  HardState hard_state_;
  PersistentState persistent_;
  RaftLog log_;
  LogIndex commit_index_{};
  struct Flight {
    LogIndex previous{};
    LogIndex last{};
  };
  struct Progress {
    LogIndex next{};
    LogIndex match{};
    std::map<std::uint64_t, Flight> flights;
  };
  std::map<NodeId, Progress> progress_;
  std::uint64_t rpc_sequence_{};
  Role role_{Role::Follower};
  std::optional<NodeId> leader_;
  std::set<NodeId> votes_;
  TimePoint election_start_{};
  TimePoint election_timeout_{};
  TimePoint last_heartbeat_{};
  bool stopped_{};
};

}  // namespace quorumkv
