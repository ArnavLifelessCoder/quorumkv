#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>

#include "quorumkv/net.hpp"
#include "quorumkv/raft_node.hpp"

namespace quorumkv {

struct ServerConfig {
  NodeId id{};
  std::vector<NodeId> members;
  // Peer and client listeners. Port 0 binds an ephemeral port; see peer_port()/client_port().
  net::Address peer_listen{.host = "127.0.0.1", .port = 0};
  net::Address client_listen{.host = "127.0.0.1", .port = 0};
  std::filesystem::path data_dir;
  // One logical Raft tick in wall-clock milliseconds. The defaults give 150 to 300 ms election
  // timeouts and 30 ms heartbeats.
  unsigned tick_ms{10};
  LogIndex snapshot_threshold{1000};
  bool unsafe_reads{};
  // Batching: commands arriving in one event-loop pass share one durable write, and one
  // AppendEntries carries up to max_append_entries entries. Pipelining: several
  // AppendEntries in flight per follower. Both can be turned off for measurement.
  bool batching{true};
  std::size_t max_append_entries{256};
  bool pipeline{true};
};

// One Raft node behind TCP. A single thread runs Run(); every socket, the clock, and the
// RaftNode belong to that thread, so several servers can share one process.
class Server {
 public:
  explicit Server(ServerConfig config);
  ~Server();
  Server(const Server&) = delete;
  auto operator=(const Server&) -> Server& = delete;
  Server(Server&&) = delete;
  auto operator=(Server&&) -> Server& = delete;

  [[nodiscard]] auto peer_port() const -> std::uint16_t;
  [[nodiscard]] auto client_port() const -> std::uint16_t;
  // Where to dial each other member. Must be set before Run().
  void SetPeers(std::map<NodeId, net::Address> peers);
  // Serves until `stop` becomes true. Throws if storage fails, which stops the node.
  void Run(const std::atomic<bool>& stop);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace quorumkv
