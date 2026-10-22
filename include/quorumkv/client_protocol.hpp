#pragma once

#include <optional>
#include <span>

#include "quorumkv/raft_node.hpp"

namespace quorumkv {

// Client frames share the peer framing (length prefix, version byte) and use tags 7 and 8.
// Peer tags 1 to 6 never appear on client connections, and vice versa.
inline constexpr std::uint8_t kClientRequestTag = 7;
inline constexpr std::uint8_t kClientResponseTag = 8;

enum class ClientKind : std::uint8_t {
  Command = 1,  // payload: kv::EncodeRequest bytes, applied through the log
  Read = 2,     // payload: kv::EncodeRead bytes, served through ReadIndex
  Status = 3,   // payload: empty
};

struct ClientRequest {
  std::uint64_t request_id{};
  ClientKind kind{ClientKind::Status};
  Bytes payload;
  auto operator==(const ClientRequest&) const -> bool = default;
};

enum class ClientStatus : std::uint8_t {
  Ok = 0,         // payload: kv::EncodeResponse bytes, or NodeStatus for Status
  NotLeader = 1,  // never proposed here; leader_hint routes the retry
  Retry = 2,      // outcome unknown: leadership changed after the request was accepted
};

struct ClientResponse {
  std::uint64_t request_id{};
  ClientStatus status{ClientStatus::Retry};
  std::optional<NodeId> leader_hint;
  Bytes payload;
  auto operator==(const ClientResponse&) const -> bool = default;
};

struct NodeStatus {
  NodeId id{};
  Role role{Role::Follower};
  Term term{};
  std::optional<NodeId> leader;
  LogIndex commit_index{};
  LogIndex last_applied{};
  LogIndex snapshot_index{};
  auto operator==(const NodeStatus&) const -> bool = default;
};

// Tag of a complete frame, or nullopt if the frame is too short to carry one.
auto FrameTag(std::span<const std::uint8_t> frame) -> std::optional<std::uint8_t>;

// Each Decode* accepts exactly one complete frame and throws CodecError otherwise.
auto EncodeClientRequest(const ClientRequest& request) -> Bytes;
auto DecodeClientRequest(std::span<const std::uint8_t> frame) -> ClientRequest;
auto EncodeClientResponse(const ClientResponse& response) -> Bytes;
auto DecodeClientResponse(std::span<const std::uint8_t> frame) -> ClientResponse;
auto EncodeNodeStatus(const NodeStatus& status) -> Bytes;
auto DecodeNodeStatus(std::span<const std::uint8_t> bytes) -> NodeStatus;

}  // namespace quorumkv
