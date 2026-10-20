#include <algorithm>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "quorumkv/codec.hpp"
#include "quorumkv/sim_cluster.hpp"

namespace quorumkv::sim {

struct Cluster::Impl {
  struct Link final : Transport {
    explicit Link(Impl& owner) : cluster(owner) {}
    Impl& cluster;
    void Send(NodeId to, const Message& message) override {
      if (to != message.to) {
        throw std::logic_error("transport recipient mismatch");
      }
      cluster.Enqueue(message);
    }
  };
  struct Host {
    Host(Impl& owner, std::uint64_t seed) : random(seed), link(owner) {}
    MemoryStorage storage;
    SeededRandom random;
    Link link;
    std::unique_ptr<RaftNode> node;
  };
  struct Packet {
    TimePoint due{};
    std::uint64_t sequence{};
    Message message;
  };
  struct Later {
    auto operator()(const Packet& left, const Packet& right) const -> bool {
      return std::tie(left.due, left.sequence) > std::tie(right.due, right.sequence);
    }
  };

  explicit Impl(ClusterConfig config) : random(config.seed), recording(config.record_trace) {
    if (config.size != 3 && config.size != 5) {
      throw std::invalid_argument("election simulator uses three or five members");
    }
    groups.resize(config.size);
    members.resize(config.size);
    std::iota(members.begin(), members.end(), NodeId{1});
    for (std::size_t index = 0; index < config.size; ++index) {
      hosts.push_back(std::make_unique<Host>(*this, random.Next()));
    }
    for (const auto id : members) {
      Boot(id);
    }
  }

  VirtualClock clock;
  SeededRandom random;
  NetworkFaults faults;
  ElectionSafety safety;
  std::vector<NodeId> members;
  std::vector<std::uint64_t> groups;
  std::vector<std::unique_ptr<Host>> hosts;
  std::priority_queue<Packet, std::vector<Packet>, Later> queue;
  SimStats stats;
  std::uint64_t sequence{};
  std::uint64_t digest{14695981039346656037ULL};
  bool recording;
  std::string trace;

  [[nodiscard]] auto At(NodeId id) const -> Host& {
    if (id == 0 || id > hosts.size()) {
      throw std::out_of_range("unknown simulated node");
    }
    return *hosts.at(static_cast<std::size_t>(id - 1));
  }

  void Hash(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      digest ^= (value >> shift) & 255U;
      digest *= 1099511628211ULL;
    }
  }

  void HashMessage(const Message& message) {
    for (const auto byte : Encode(message)) {
      Hash(byte);
    }
  }

  void Check(std::string_view event, NodeId id = 0) {
    ++stats.steps;
    Hash(clock.Now());
    Hash(id);
    for (const auto character : event) {
      Hash(static_cast<unsigned char>(character));
    }
    for (const auto member : members) {
      const auto& host = At(member);
      const auto state = host.storage.hard_state();
      const auto role = host.node ? host.node->role() : Role::Follower;
      safety.Observe(member, role, state);
      if (host.node && host.node->hard_state() != state) {
        throw std::logic_error("node hard state differs from durable storage");
      }
      Hash(member);
      Hash(host.node != nullptr ? 1 : 0);
      Hash(static_cast<std::uint64_t>(role));
      Hash(state.term);
      Hash(state.voted_for.value_or(0));
    }
    ++stats.safety_checks;
    if (recording) {
      std::ostringstream line;
      line << stats.steps << ' ' << clock.Now() << ' ' << event << ' ' << id << ' ' << std::hex
           << digest << '\n';
      trace += line.str();
    }
  }

  [[nodiscard]] auto Connected(const Message& message) const -> bool {
    return groups.at(static_cast<std::size_t>(message.from - 1)) ==
           groups.at(static_cast<std::size_t>(message.to - 1));
  }

  void Enqueue(const Message& message) {
    HashMessage(message);
    if (!Connected(message) || random.Next() % 100 < faults.drop_percent) {
      ++stats.dropped;
      Hash(0);
      return;
    }
    const bool duplicate = random.Next() % 100 < faults.duplicate_percent;
    if (duplicate) {
      ++stats.duplicated;
    }
    for (unsigned copy = 0; copy < (duplicate ? 2U : 1U); ++copy) {
      const auto due = clock.Now() + 1 + random.Next() % faults.max_delay;
      Hash(due);
      queue.push(Packet{.due = due, .sequence = sequence++, .message = message});
    }
  }

  void Boot(NodeId id) {
    auto& host = At(id);
    if (host.node) {
      return;
    }
    host.node = std::make_unique<RaftNode>(ElectionConfig{.id = id, .members = members}, host.link,
                                           clock, host.storage, host.random);
    ++stats.restarts;
    Check("restart", id);
  }

  void Crash(NodeId id) {
    auto& host = At(id);
    host.node.reset();
    host.storage.Crash();
    ++stats.crashes;
    Check("crash", id);
  }

  template <class Operation>
  void Run(NodeId id, Operation operation) {
    auto& host = At(id);
    if (!host.node) {
      return;
    }
    try {
      operation(*host.node);
    } catch (const std::runtime_error&) {
      if (!host.node->stopped()) {
        throw;
      }
      ++stats.sync_failures;
      Crash(id);
    }
  }

  void Advance() {
    clock.Advance();
    Check("clock");
    auto order = members;
    for (std::size_t count = order.size(); count > 1; --count) {
      std::swap(order[count - 1], order[static_cast<std::size_t>(random.Next() % count)]);
    }
    for (const auto id : order) {
      if (At(id).node) {
        Run(id, [](RaftNode& node) { node.Tick(); });
        Check("tick", id);
      }
    }
    while (!queue.empty() && queue.top().due <= clock.Now()) {
      const auto packet = queue.top();
      queue.pop();
      HashMessage(packet.message);
      if (At(packet.message.to).node && Connected(packet.message)) {
        ++stats.delivered;
        Run(packet.message.to, [&packet](RaftNode& node) { node.Step(packet.message); });
        Check("deliver", packet.message.to);
      } else {
        ++stats.dropped;
        Check("drop", packet.message.to);
      }
    }
  }
};

Cluster::Cluster(ClusterConfig config) : impl_(std::make_unique<Impl>(config)) {}
Cluster::~Cluster() = default;
void Cluster::Advance() { impl_->Advance(); }
void Cluster::Crash(NodeId id) { impl_->Crash(id); }
void Cluster::Restart(NodeId id) { impl_->Boot(id); }
void Cluster::FailNextSync(NodeId id, SyncFailure failure) {
  impl_->At(id).storage.FailNext(failure);
  impl_->Hash(static_cast<std::uint64_t>(failure));
  impl_->Check("arm-sync-failure", id);
}
void Cluster::Partition(const std::vector<std::uint64_t>& groups) {
  if (groups.size() != impl_->members.size()) {
    throw std::invalid_argument("one partition label is required per member");
  }
  impl_->groups = groups;
  for (const auto group : groups) {
    impl_->Hash(group);
  }
  ++impl_->stats.partitions;
  impl_->Check("partition");
}
void Cluster::Heal() {
  std::ranges::fill(impl_->groups, 0);
  impl_->Check("heal");
}
void Cluster::SetFaults(NetworkFaults faults) {
  if (faults.drop_percent > 100 || faults.duplicate_percent > 100 || faults.max_delay == 0 ||
      faults.max_delay > 1000) {
    throw std::invalid_argument("invalid network faults");
  }
  impl_->faults = faults;
  impl_->Hash(faults.drop_percent);
  impl_->Hash(faults.duplicate_percent);
  impl_->Hash(faults.max_delay);
  impl_->Check("faults");
}
auto Cluster::Leaders() const -> std::vector<NodeId> {
  std::vector<NodeId> leaders;
  for (const auto id : impl_->members) {
    if (const auto& node = impl_->At(id).node; node && node->role() == Role::Leader) {
      leaders.push_back(id);
    }
  }
  return leaders;
}
auto Cluster::Alive(NodeId id) const -> bool { return impl_->At(id).node != nullptr; }
auto Cluster::State(NodeId id) const -> HardState { return impl_->At(id).storage.hard_state(); }
auto Cluster::Stats() const -> SimStats { return impl_->stats; }
auto Cluster::Digest() const -> std::uint64_t { return impl_->digest; }
auto Cluster::Trace() const -> const std::string& { return impl_->trace; }

}  // namespace quorumkv::sim
