#include "quorumkv/server.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <thread>

#include "quorumkv/client.hpp"

namespace quorumkv {
namespace {

using namespace std::chrono_literals;

// Three servers on ephemeral localhost ports, each driven by its own thread. Servers share no
// state; all traffic between them is real TCP.
class LocalCluster {
 public:
  explicit LocalCluster(const std::string& name)
      : root_(std::filesystem::path(QUORUMKV_TEST_ROOT) / "server-tests" / name) {
    std::filesystem::remove_all(root_);
    for (NodeId id = 1; id <= 3; ++id) {
      Boot(id);
    }
    Connect();
  }
  ~LocalCluster() {
    try {
      for (NodeId id = 1; id <= 3; ++id) {
        Stop(id);
      }
    } catch (...) {
      ADD_FAILURE() << "a server thread failed to stop";
    }
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  LocalCluster(const LocalCluster&) = delete;
  auto operator=(const LocalCluster&) -> LocalCluster& = delete;
  LocalCluster(LocalCluster&&) = delete;
  auto operator=(LocalCluster&&) -> LocalCluster& = delete;

  void Stop(NodeId id) {
    auto& node = nodes_.at(id - 1);
    if (node.thread.joinable()) {
      node.stop = true;
      node.thread.join();
    }
    node.server.reset();
  }

  // Restarts on the same data directory and ports, so the others reconnect by themselves.
  void Restart(NodeId id) {
    Boot(id);
    Start(id);
  }

  void Start(NodeId id) {
    auto& node = nodes_.at(id - 1);
    node.stop = false;
    node.thread = std::thread([&node] { node.server->Run(node.stop); });
  }

  void Connect() {
    std::map<NodeId, net::Address> peers;
    for (NodeId id = 1; id <= 3; ++id) {
      peers[id] = {.host = "127.0.0.1", .port = nodes_.at(id - 1).peer_port};
    }
    for (NodeId id = 1; id <= 3; ++id) {
      nodes_.at(id - 1).server->SetPeers(peers);
      Start(id);
    }
  }

  auto Addresses() const -> std::vector<net::Address> {
    std::vector<net::Address> addresses;
    addresses.reserve(nodes_.size());
    for (const auto& node : nodes_) {
      addresses.push_back({.host = "127.0.0.1", .port = node.client_port});
    }
    return addresses;
  }

 private:
  struct Node {
    std::unique_ptr<Server> server;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::uint16_t peer_port{};
    std::uint16_t client_port{};
  };

  void Boot(NodeId id) {
    auto& node = nodes_.at(id - 1);
    ServerConfig config{.id = id,
                        .members = {1, 2, 3},
                        .peer_listen = {.host = "127.0.0.1", .port = node.peer_port},
                        .client_listen = {.host = "127.0.0.1", .port = node.client_port},
                        .data_dir = root_ / std::to_string(id),
                        .tick_ms = 5,
                        .snapshot_threshold = 20,
                        .unsafe_reads = false};
    std::filesystem::create_directories(config.data_dir);
    const bool rebooting = node.peer_port != 0;
    node.server = std::make_unique<Server>(config);
    node.peer_port = node.server->peer_port();
    node.client_port = node.server->client_port();
    if (rebooting) {
      std::map<NodeId, net::Address> peers;
      for (NodeId other = 1; other <= 3; ++other) {
        peers[other] = {.host = "127.0.0.1", .port = nodes_.at(other - 1).peer_port};
      }
      node.server->SetPeers(peers);
    }
  }

  std::filesystem::path root_;
  std::array<Node, 3> nodes_;
};

auto Value(const Result& result) -> std::string {
  EXPECT_EQ(result.outcome, Outcome::Ok);
  const auto* value = std::get_if<kv::Value>(&result.response);
  return value != nullptr && value->found ? value->value : "<missing>";
}

auto LeaderOf(Client& client) -> NodeId {
  for (int attempt = 0; attempt < 200; ++attempt) {
    for (NodeId id = 1; id <= 3; ++id) {
      if (const auto status = client.Status(id); status && status->role == Role::Leader) {
        return id;
      }
    }
    std::this_thread::sleep_for(20ms);
  }
  return 0;
}

TEST(Server, ClientOperationsReplicateOverTcpAndSurviveLeaderFailover) {
  LocalCluster cluster("failover");
  Client client({.nodes = cluster.Addresses(),
                 .id = {.high = 1, .low = 1},
                 .attempt_timeout = 300ms,
                 .operation_timeout = 10s});
  ASSERT_EQ(client.Execute(kv::Put{.key = "k", .value = "1"}).outcome, Outcome::Ok);
  ASSERT_EQ(client.Execute(kv::Append{.key = "k", .suffix = "2"}).outcome, Outcome::Ok);
  const auto cas = client.Execute(kv::Cas{.key = "k", .expected = "12", .replacement = "3"});
  ASSERT_EQ(cas.outcome, Outcome::Ok);
  EXPECT_TRUE(std::get<kv::CasResult>(cas.response).exchanged);
  EXPECT_EQ(Value(client.Read("k")), "3");
  EXPECT_EQ(Value(client.Execute(kv::Get{.key = "k"})), "3");

  const auto leader = LeaderOf(client);
  ASSERT_NE(leader, 0U);
  cluster.Stop(leader);
  // A new leader is elected; writes and ReadIndex reads continue.
  ASSERT_EQ(client.Execute(kv::Append{.key = "k", .suffix = "4"}).outcome, Outcome::Ok);
  EXPECT_EQ(Value(client.Read("k")), "34");
  const auto next = LeaderOf(client);
  EXPECT_NE(next, 0U);
  EXPECT_NE(next, leader);

  // The old leader restarts from its data directory and catches up.
  cluster.Restart(leader);
  for (int i = 0; i < 30; ++i) {
    ASSERT_EQ(client.Execute(kv::Append{.key = "n", .suffix = "x"}).outcome, Outcome::Ok);
  }
  bool caught_up = false;
  for (int attempt = 0; attempt < 200 && !caught_up; ++attempt) {
    const auto restarted = client.Status(leader);
    const auto current = client.Status(LeaderOf(client));
    caught_up = restarted && current && restarted->last_applied == current->last_applied;
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_TRUE(caught_up);
  EXPECT_EQ(Value(client.Read("n")), std::string(30, 'x'));
}

TEST(Server, FollowersRedirectAndNoQuorumMeansNoWrites) {
  LocalCluster cluster("quorum");
  Client client({.nodes = cluster.Addresses(),
                 .id = {.high = 1, .low = 2},
                 .attempt_timeout = 200ms,
                 .operation_timeout = 10s});
  ASSERT_EQ(client.Execute(kv::Put{.key = "q", .value = "1"}).outcome, Outcome::Ok);
  const auto leader = LeaderOf(client);
  ASSERT_NE(leader, 0U);
  for (NodeId id = 1; id <= 3; ++id) {
    if (id != leader) {
      cluster.Stop(id);
    }
  }
  Client hopeless({.nodes = cluster.Addresses(),
                   .id = {.high = 1, .low = 3},
                   .attempt_timeout = 200ms,
                   .operation_timeout = 1500ms});
  // Without a majority the write cannot commit, and the read cannot confirm leadership.
  EXPECT_EQ(hopeless.Execute(kv::Put{.key = "q", .value = "2"}).outcome, Outcome::Unknown);
  EXPECT_EQ(hopeless.Read("q").outcome, Outcome::Fail);
}

}  // namespace
}  // namespace quorumkv
