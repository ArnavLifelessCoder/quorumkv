#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace quorumkv {

using NodeId = std::uint64_t;
using Term = std::uint64_t;
using LogIndex = std::uint64_t;
using TimePoint = std::uint64_t;
using Bytes = std::vector<std::uint8_t>;

enum class EntryKind : std::uint8_t { NoOp = 0, Command = 1 };

struct LogEntry {
  LogIndex index{};
  Term term{};
  EntryKind kind{EntryKind::NoOp};
  Bytes command;
  bool operator==(const LogEntry&) const = default;
};

struct RequestVote {
  LogIndex last_log_index{};
  Term last_log_term{};
  bool operator==(const RequestVote&) const = default;
};

struct VoteResponse {
  bool granted{};
  bool operator==(const VoteResponse&) const = default;
};

struct AppendEntries {
  LogIndex prev_log_index{};
  Term prev_log_term{};
  LogIndex leader_commit{};
  std::uint64_t read_context{};
  std::vector<LogEntry> entries;
  bool operator==(const AppendEntries&) const = default;
};

struct AppendResponse {
  bool success{};
  LogIndex match_index{};
  std::optional<Term> conflict_term;
  LogIndex conflict_index{};
  std::uint64_t read_context{};
  bool operator==(const AppendResponse&) const = default;
};

struct InstallSnapshot {
  LogIndex last_included_index{};
  Term last_included_term{};
  std::uint64_t offset{};
  bool done{};
  Bytes data;
  bool operator==(const InstallSnapshot&) const = default;
};

struct SnapshotResponse {
  bool success{};
  std::uint64_t next_offset{};
  bool operator==(const SnapshotResponse&) const = default;
};

using MessageBody = std::variant<RequestVote, VoteResponse, AppendEntries, AppendResponse,
                                 InstallSnapshot, SnapshotResponse>;

struct Message {
  NodeId from{};
  NodeId to{};
  Term term{};
  std::uint64_t rpc_id{};
  MessageBody body;
  bool operator==(const Message&) const = default;
};

struct HardState {
  Term term{};
  std::optional<NodeId> voted_for;
  bool operator==(const HardState&) const = default;
};

struct Snapshot {
  LogIndex last_included_index{};
  Term last_included_term{};
  Bytes state;
  bool operator==(const Snapshot&) const = default;
};

struct PersistentState {
  HardState hard_state;
  std::vector<LogEntry> entries;
  std::optional<Snapshot> snapshot;
  bool operator==(const PersistentState&) const = default;
};

struct StorageBatch {
  std::optional<HardState> hard_state;
  std::optional<LogIndex> truncate_from;
  std::vector<LogEntry> append;
  std::optional<Snapshot> snapshot;
};

}  // namespace quorumkv
