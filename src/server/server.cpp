#include "quorumkv/server.hpp"

#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>

#include "quorumkv/client_protocol.hpp"
#include "quorumkv/codec.hpp"
#include "quorumkv/file_storage.hpp"
#include "quorumkv/kv.hpp"

namespace quorumkv {
namespace {

using SteadyClock = std::chrono::steady_clock;

// Logical ticks of `tick_ms` since the server started. Monotonic, with no wall-clock epoch.
class TickClock final : public Clock {
 public:
  explicit TickClock(unsigned tick_ms) : tick_(tick_ms), start_(SteadyClock::now()) {}
  auto Now() -> TimePoint override {
    return static_cast<TimePoint>((SteadyClock::now() - start_) / tick_);
  }

 private:
  std::chrono::milliseconds tick_;
  SteadyClock::time_point start_;
};

// SplitMix64 seeded once per process; the node never touches a global generator.
class ProcessRandom final : public Random {
 public:
  ProcessRandom()
      : state_((static_cast<std::uint64_t>(std::random_device{}()) << 32U) ^
               static_cast<std::uint64_t>(SteadyClock::now().time_since_epoch().count())) {}
  auto Next() -> std::uint64_t override {
    auto value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

constexpr std::size_t kMaxQueuedBytes = std::size_t{8} * 1024 * 1024;
constexpr auto kReconnectDelay = std::chrono::milliseconds(100);

struct Connection {
  net::Socket socket;
  net::FrameReader reader;
  Bytes out;
  bool connecting{};
};

}  // namespace

struct Server::Impl {
  struct Link final : Transport {
    explicit Link(Impl& owner) : server(owner) {}
    Impl& server;
    void Send(NodeId to, const Message& message) override { server.QueuePeer(to, message); }
  };
  struct PendingWrite {
    std::uint64_t connection{};
    std::uint64_t request{};
    Term term{};
  };
  struct PendingRead {
    std::uint64_t connection{};
    std::uint64_t request{};
  };
  struct Outbound {
    net::Address address;
    Connection connection;
    SteadyClock::time_point retry_at{};
  };

  explicit Impl(ServerConfig server_config)
      : config(std::move(server_config)),
        clock(config.tick_ms),
        storage(config.data_dir),
        link(*this),
        peer_listener(net::Listen(config.peer_listen)),
        client_listener(net::Listen(config.client_listen)),
        node(ElectionConfig{.id = config.id,
                            .members = config.members,
                            .max_append_entries = config.batching ? config.max_append_entries : 1,
                            .snapshot_threshold = config.snapshot_threshold,
                            .pipeline = config.pipeline,
                            .unsafe_local_reads = config.unsafe_reads},
             link, clock, storage, random, &machine) {}

  ServerConfig config;
  TickClock clock;
  ProcessRandom random;
  FileStorage storage;
  kv::Store machine;
  Link link;
  net::Socket peer_listener;
  net::Socket client_listener;
  RaftNode node;
  std::map<NodeId, Outbound> peers;
  std::map<std::uint64_t, Connection> inbound_peers;
  std::map<std::uint64_t, Connection> clients;
  std::map<LogIndex, PendingWrite> writes;
  std::map<std::uint64_t, PendingRead> reads;
  std::uint64_t next_connection{1};
  std::uint64_t next_read{1};
  struct QueuedCommand {
    std::uint64_t connection{};
    std::uint64_t request{};
    Bytes payload;
  };
  std::vector<QueuedCommand> queued;

  // Proposes every command received in this event-loop pass under one durable write.
  void ProposeQueued() {
    if (queued.empty()) {
      return;
    }
    auto commands = std::exchange(queued, {});
    if (node.role() != Role::Leader) {
      for (const auto& command : commands) {
        NotLeader(command.connection, command.request);
      }
      return;
    }
    const auto term = node.hard_state().term;
    std::vector<Bytes> payloads;
    payloads.reserve(commands.size());
    for (auto& command : commands) {
      payloads.push_back(std::move(command.payload));
    }
    const auto indices = node.ProposeBatch(std::move(payloads));
    for (std::size_t index = 0; index < indices.size(); ++index) {
      writes[indices[index]] = {.connection = commands[index].connection,
                                .request = commands[index].request,
                                .term = term};
    }
    Drain();
  }

  void QueuePeer(NodeId to, const Message& message) {
    const auto found = peers.find(to);
    if (found == peers.end()) {
      return;
    }
    auto& connection = found->second.connection;
    // A dropped message is indistinguishable from network loss, which Raft tolerates.
    if (!connection.socket.valid() || connection.connecting ||
        connection.out.size() > kMaxQueuedBytes) {
      return;
    }
    const auto frame = Encode(message);
    connection.out.insert(connection.out.end(), frame.begin(), frame.end());
  }

  void Respond(std::uint64_t connection, const ClientResponse& response) {
    const auto found = clients.find(connection);
    if (found == clients.end()) {
      return;
    }
    const auto frame = EncodeClientResponse(response);
    found->second.out.insert(found->second.out.end(), frame.begin(), frame.end());
  }

  void NotLeader(std::uint64_t connection, std::uint64_t request) {
    Respond(connection, {.request_id = request,
                         .status = ClientStatus::NotLeader,
                         .leader_hint = node.leader_hint(),
                         .payload = {}});
  }

  void Drain() {
    for (const auto& applied : node.TakeApplied()) {
      const auto found = writes.find(applied.index);
      if (found == writes.end()) {
        continue;
      }
      const auto pending = found->second;
      writes.erase(found);
      // A different term at this index means another leader's entry replaced ours.
      const bool ours = pending.term == applied.term;
      Respond(pending.connection, {.request_id = pending.request,
                                   .status = ours ? ClientStatus::Ok : ClientStatus::Retry,
                                   .leader_hint = std::nullopt,
                                   .payload = ours ? applied.response : Bytes{}});
    }
    // Entries covered by an installed snapshot are never reported one by one.
    while (!writes.empty() && writes.begin()->first <= node.last_applied()) {
      const auto pending = writes.begin()->second;
      writes.erase(writes.begin());
      Respond(pending.connection, {.request_id = pending.request,
                                   .status = ClientStatus::Retry,
                                   .leader_hint = std::nullopt,
                                   .payload = {}});
    }
    for (auto& read : node.TakeReads()) {
      const auto found = reads.find(read.id);
      if (found == reads.end()) {
        continue;
      }
      const auto pending = found->second;
      reads.erase(found);
      Respond(pending.connection, {.request_id = pending.request,
                                   .status = read.ok ? ClientStatus::Ok : ClientStatus::Retry,
                                   .leader_hint = read.ok ? std::nullopt : node.leader_hint(),
                                   .payload = std::move(read.response)});
    }
  }

  void HandleClient(std::uint64_t connection, const ClientRequest& request) {
    switch (request.kind) {
      case ClientKind::Status: {
        const NodeStatus status{.id = config.id,
                                .role = node.role(),
                                .term = node.hard_state().term,
                                .leader = node.leader_hint(),
                                .commit_index = node.commit_index(),
                                .last_applied = node.last_applied(),
                                .snapshot_index = node.log().base_index()};
        Respond(connection, {.request_id = request.request_id,
                             .status = ClientStatus::Ok,
                             .leader_hint = node.leader_hint(),
                             .payload = EncodeNodeStatus(status)});
        return;
      }
      case ClientKind::Command: {
        if (config.batching) {
          queued.push_back({.connection = connection,
                            .request = request.request_id,
                            .payload = request.payload});
          return;
        }
        const auto term = node.hard_state().term;
        const auto index =
            node.role() == Role::Leader ? node.Propose(request.payload) : std::optional<LogIndex>{};
        if (!index) {
          NotLeader(connection, request.request_id);
          return;
        }
        writes[*index] = {.connection = connection, .request = request.request_id, .term = term};
        return;
      }
      case ClientKind::Read: {
        const auto id = next_read++;
        reads[id] = {.connection = connection, .request = request.request_id};
        if (!node.Read(id, request.payload)) {
          reads.erase(id);
          NotLeader(connection, request.request_id);
        }
        return;
      }
    }
  }

  // Returns false when the connection must be dropped.
  auto ReadFrames(Connection& connection, bool from_client, std::uint64_t id) -> bool {
    Bytes incoming;
    const auto result = net::Receive(connection.socket, incoming);
    connection.reader.Append(incoming);
    try {
      while (auto frame = connection.reader.Next()) {
        const auto tag = FrameTag(*frame);
        if (from_client) {
          if (tag != kClientRequestTag) {
            return false;
          }
          HandleClient(id, DecodeClientRequest(*frame));
        } else {
          node.Step(Decode(*frame));
        }
        Drain();
      }
    } catch (const CodecError&) {
      return false;  // A malformed frame ends the connection, never the node.
    }
    return result != net::IoResult::Closed;
  }

  void Accept(const net::Socket& listener, std::map<std::uint64_t, Connection>& into) {
    for (;;) {
      auto socket = net::Accept(listener);
      if (!socket.valid()) {
        return;
      }
      into.emplace(
          next_connection++,
          Connection{.socket = std::move(socket), .reader = {}, .out = {}, .connecting = false});
    }
  }

  void Reconnect(SteadyClock::time_point now) {
    for (auto& [id, peer] : peers) {
      static_cast<void>(id);
      if (!peer.connection.socket.valid() && now >= peer.retry_at) {
        peer.connection = Connection{
            .socket = net::Connect(peer.address), .reader = {}, .out = {}, .connecting = true};
        peer.retry_at = now + kReconnectDelay;
      }
    }
  }

  void Loop(const std::atomic<bool>& stop) {
    auto next_tick = SteadyClock::now();
    while (!stop.load()) {
      auto now = SteadyClock::now();
      Reconnect(now);
      std::vector<net::PollEntry> entries;
      entries.push_back({.socket = &peer_listener, .want_read = true});
      entries.push_back({.socket = &client_listener, .want_read = true});
      for (auto& [id, peer] : peers) {
        static_cast<void>(id);
        if (peer.connection.socket.valid()) {
          entries.push_back(
              {.socket = &peer.connection.socket,
               .want_read = !peer.connection.connecting,
               .want_write = peer.connection.connecting || !peer.connection.out.empty()});
        }
      }
      for (auto* group : {&inbound_peers, &clients}) {
        for (auto& [id, connection] : *group) {
          static_cast<void>(id);
          entries.push_back({.socket = &connection.socket,
                             .want_read = true,
                             .want_write = !connection.out.empty()});
        }
      }
      const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now);
      net::Poll(entries, static_cast<int>(
                             std::max<std::int64_t>(0, std::min<std::int64_t>(wait.count(), 50))));
      std::size_t cursor = 0;
      // New connections are accepted only after every polled entry has been consumed.
      const bool peer_pending = entries[cursor++].readable;
      const bool client_pending = entries[cursor++].readable;
      for (auto& [id, peer] : peers) {
        static_cast<void>(id);
        auto& connection = peer.connection;
        if (!connection.socket.valid()) {
          continue;
        }
        const auto& entry = entries[cursor++];
        if (connection.connecting && (entry.writable || entry.failed)) {
          connection.connecting = false;
          if (entry.failed || !net::Connected(connection.socket)) {
            connection.socket.Close();
            continue;
          }
        }
        bool alive = !entry.failed;
        if (alive && entry.readable) {
          // Replies arrive on the dialed peer's own connection; nothing is read here.
          Bytes discard;
          alive = net::Receive(connection.socket, discard) != net::IoResult::Closed;
        }
        if (alive && !connection.out.empty()) {
          alive = net::SendSome(connection.socket, connection.out) != net::IoResult::Closed;
        }
        if (!alive) {
          connection = Connection{};
        }
      }
      for (auto* group : {&inbound_peers, &clients}) {
        const bool from_client = group == &clients;
        for (auto it = group->begin(); it != group->end();) {
          const auto& entry = entries[cursor++];
          bool alive = !entry.failed;
          if (alive && entry.readable) {
            alive = ReadFrames(it->second, from_client, it->first);
          }
          if (alive && !it->second.out.empty()) {
            alive = net::SendSome(it->second.socket, it->second.out) != net::IoResult::Closed;
          }
          it = alive ? std::next(it) : group->erase(it);
        }
      }
      ProposeQueued();
      if (peer_pending) {
        Accept(peer_listener, inbound_peers);
      }
      if (client_pending) {
        Accept(client_listener, clients);
      }
      now = SteadyClock::now();
      if (now >= next_tick) {
        node.Tick();
        Drain();
        next_tick = now + std::chrono::milliseconds(config.tick_ms);
      }
    }
  }
};

Server::Server(ServerConfig config)
    : impl_((net::Initialize(), std::make_unique<Impl>(std::move(config)))) {}
Server::~Server() = default;
auto Server::peer_port() const -> std::uint16_t { return net::LocalPort(impl_->peer_listener); }
auto Server::client_port() const -> std::uint16_t { return net::LocalPort(impl_->client_listener); }
void Server::SetPeers(std::map<NodeId, net::Address> peers) {
  impl_->peers.clear();
  for (auto& [id, address] : peers) {
    if (id != impl_->config.id) {
      impl_->peers.emplace(
          id, Impl::Outbound{.address = std::move(address), .connection = {}, .retry_at = {}});
    }
  }
}
void Server::Run(const std::atomic<bool>& stop) { impl_->Loop(stop); }

}  // namespace quorumkv
