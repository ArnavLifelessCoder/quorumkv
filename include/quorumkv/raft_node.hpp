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
  // Compact once this many applied entries are retained. Zero disables snapshots.
  LogIndex snapshot_threshold{};
  std::size_t snapshot_chunk_bytes{std::size_t{64} * 1024};
  // False sends one AppendEntries per follower at a time; only the heartbeat timer
  // retransmits. Used to measure what pipelining is worth.
  bool pipeline{true};
  // Deliberately unsafe: serve reads from local state on any node without ReadIndex.
  // Exists only so the history checkers can prove they detect stale reads.
  bool unsafe_local_reads{};
};

// The result of applying one committed command, in log order.
struct Applied {
  LogIndex index{};
  Term term{};
  Bytes response;
  auto operator==(const Applied&) const -> bool = default;
};

// `ok` is false when leadership was lost before the read could be served safely.
struct ReadResult {
  std::uint64_t id{};
  bool ok{};
  Bytes response;
  auto operator==(const ReadResult&) const -> bool = default;
};

// Callers serialize Step/Tick/Propose/Read and stop on exceptions.
class RaftNode {
 public:
  // `machine` may be null for protocol-only use. It must start empty: entries are
  // reapplied from the retained log after every restart.
  RaftNode(ElectionConfig config, Transport& transport, Clock& clock, Storage& storage,
           Random& random, StateMachine* machine = nullptr);
  void Step(const Message& message);
  void Tick();
  auto Propose(Bytes command) -> std::optional<LogIndex>;
  // Appends every command under one durable write (group commit). Empty if not leader.
  auto ProposeBatch(std::vector<Bytes> commands) -> std::vector<LogIndex>;
  // Linearizable read through ReadIndex. Returns false when this node is not leader.
  auto Read(std::uint64_t id, Bytes query) -> bool;
  auto TakeApplied() -> std::vector<Applied>;
  auto TakeReads() -> std::vector<ReadResult>;
  [[nodiscard]] auto last_applied() const -> LogIndex { return last_applied_; }
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
  void SendAppend(NodeId peer, bool retransmit = true);
  auto AdvanceCommit() -> bool;
  void ResetElectionTimer();
  void StartElection();
  void BecomeLeader();
  void Heartbeat(bool timer = true);
  void Send(NodeId to, MessageBody body, std::uint64_t rpc_id = 0);
  void Receive(const Message& message, const RequestVote& vote);
  void Receive(const Message& message, const VoteResponse& vote);
  void Receive(const Message& message, const AppendEntries& append);
  void Receive(const Message& message, const AppendResponse& reply);
  void Receive(const Message& message, const InstallSnapshot& install);
  void Receive(const Message& message, const SnapshotResponse& reply);
  void SendSnapshot(NodeId peer);
  void MaybeCompact();
  void EnsureRunning() const;
  void ApplyCommitted();
  void ServeReads();
  void FailReads();
  [[nodiscard]] auto ReadConfirmed(std::uint64_t context) const -> bool;

  ElectionConfig config_;
  Transport& transport_;
  Clock& clock_;
  Storage& storage_;
  Random& random_;
  StateMachine* machine_;
  HardState hard_state_;
  PersistentState persistent_;
  RaftLog log_;
  LogIndex commit_index_{};
  struct Flight {
    LogIndex previous{};
    LogIndex last{};
  };
  struct Progress {
    explicit Progress(LogIndex first) : next(first) {}
    Progress(const Progress&) = delete;
    auto operator=(const Progress&) -> Progress& = delete;
    LogIndex next{};
    LogIndex match{};
    // Highest read context echoed by this peer in the current term.
    std::uint64_t acked_context{};
    std::map<std::uint64_t, Flight> flights;
    // One chunk is outstanding at a time; heartbeats resend it until it is acknowledged.
    struct SnapshotTransfer {
      LogIndex index{};
      std::uint64_t offset{};
      std::uint64_t rpc{};
    };
    std::optional<SnapshotTransfer> snapshot;
  };
  // Chunks of a snapshot being received, accepted only in order.
  struct IncomingSnapshot {
    LogIndex index{};
    Term term{};
    Bytes data;
  };
  std::optional<IncomingSnapshot> incoming_;
  struct PendingRead {
    std::uint64_t id{};
    Bytes query;
    std::uint64_t context{};
    std::optional<LogIndex> read_index;
  };
  std::map<NodeId, Progress> progress_;
  std::uint64_t rpc_sequence_{};
  LogIndex last_applied_{};
  std::vector<Applied> applied_;
  std::vector<PendingRead> reads_;
  std::vector<ReadResult> read_results_;
  std::uint64_t read_context_{};
  Role role_{Role::Follower};
  std::optional<NodeId> leader_;
  std::set<NodeId> votes_;
  TimePoint election_start_{};
  TimePoint election_timeout_{};
  TimePoint last_heartbeat_{};
  bool stopped_{};
};

}  // namespace quorumkv
