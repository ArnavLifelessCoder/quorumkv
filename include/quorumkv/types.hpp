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
  auto operator==(const LogEntry&) const -> bool = default;
};

struct RequestVote {
  LogIndex last_log_index{};
  Term last_log_term{};
  auto operator==(const RequestVote&) const -> bool = default;
};

struct VoteResponse {
  bool granted{};
  auto operator==(const VoteResponse&) const -> bool = default;
};

struct AppendEntries {
  LogIndex prev_log_index{};
  Term prev_log_term{};
  LogIndex leader_commit{};
  std::uint64_t read_context{};
  std::vector<LogEntry> entries;
  auto operator==(const AppendEntries&) const -> bool = default;
};

struct AppendResponse {
  bool success{};
  LogIndex match_index{};
  std::optional<Term> conflict_term;
  LogIndex conflict_index{};
  std::uint64_t read_context{};
  auto operator==(const AppendResponse&) const -> bool = default;
};

struct InstallSnapshot {
  LogIndex last_included_index{};
  Term last_included_term{};
  std::uint64_t offset{};
  bool done{};
  Bytes data;
  auto operator==(const InstallSnapshot&) const -> bool = default;
};

struct SnapshotResponse {
  bool success{};
  std::uint64_t next_offset{};
  auto operator==(const SnapshotResponse&) const -> bool = default;
};

using MessageBody = std::variant<RequestVote, VoteResponse, AppendEntries, AppendResponse,
                                 InstallSnapshot, SnapshotResponse>;

struct Message {
  NodeId from{};
  NodeId to{};
  Term term{};
  std::uint64_t rpc_id{};
  MessageBody body;
  auto operator==(const Message&) const -> bool = default;
};

struct HardState {
  Term term{};
  std::optional<NodeId> voted_for;
  auto operator==(const HardState&) const -> bool = default;
};

struct Snapshot {
  LogIndex last_included_index{};
  Term last_included_term{};
  Bytes state;
  auto operator==(const Snapshot&) const -> bool = default;
};

struct PersistentState {
  HardState hard_state;
  std::vector<LogEntry> entries;
  std::optional<Snapshot> snapshot;
  auto operator==(const PersistentState&) const -> bool = default;
};

struct StorageBatch {
  std::optional<HardState> hard_state;
  std::optional<LogIndex> truncate_from;
  std::vector<LogEntry> append;
  std::optional<Snapshot> snapshot;
};

}  // namespace quorumkv
