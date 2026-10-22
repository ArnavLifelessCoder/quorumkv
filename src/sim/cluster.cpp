#include <algorithm>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "quorumkv/codec.hpp"
#include "quorumkv/replication_safety.hpp"
#include "quorumkv/sim_cluster.hpp"

namespace quorumkv::sim {

struct Cluster::Impl {
  struct PowerLoss final : std::exception {};
  struct Link final : Transport {
    explicit Link(Impl& owner) : cluster(owner) {}
    Impl& cluster;
    void Send(NodeId to, const Message& message) override {
      if (to != message.to) {
        throw std::logic_error("transport recipient mismatch");
      }
      cluster.Enqueue(message);
      if (std::holds_alternative<RequestVote>(message.body) &&
          cluster.random.Next() % 100 < cluster.candidate_crash_percent) {
        ++cluster.stats.candidate_crashes;
        const auto durable = cluster.At(message.from).storage.hard_state();
        if (durable.term != message.term || durable.voted_for != message.from) {
          throw std::logic_error("candidate crash loses announced term or self-vote");
        }
        throw PowerLoss{};
      }
      if (const auto* append = std::get_if<AppendResponse>(&message.body);
          append && append->success && cluster.random.Next() % 100 < cluster.append_crash_percent) {
        ++cluster.stats.append_reply_crashes;
        throw PowerLoss{};
      }
      if (const auto* vote = std::get_if<VoteResponse>(&message.body);
          vote && vote->granted && cluster.random.Next() % 100 < cluster.vote_crash_percent) {
        // Unwind the current Step before any later instruction can run. The driver
        // destroys the node; Transport never calls back into it.
        ++cluster.stats.vote_grant_crashes;
        throw PowerLoss{};
      }
    }
  };
  struct Host {
    Host(Impl& owner, std::uint64_t seed) : random(seed), link(owner) {}
    MemoryStorage storage;
    SeededRandom random;
    Link link;
    // Declared before the node, so the node is always destroyed first.
    std::unique_ptr<StateMachine> machine;
    std::unique_ptr<RaftNode> node;
    std::optional<std::uint64_t> checked_revision;
    RaftLog checked_log;
    LogIndex applied{};
    std::uint64_t epoch{};
    LogIndex checked_applied{};
    std::vector<NodeApplied> applied_out;
    std::vector<NodeRead> reads_out;
  };

  explicit Impl(const ClusterConfig& config)
      : random(config.seed),
        recording(config.record_trace),
        manual_delivery(config.manual_delivery),
        max_append_entries(config.max_append_entries),
        machine_factory(config.machine_factory),
        unsafe_reads(config.unsafe_reads),
        snapshot_threshold(config.snapshot_threshold),
        snapshot_chunk_bytes(config.snapshot_chunk_bytes) {
    if (config.size != 3 && config.size != 5) {
      throw std::invalid_argument("election simulator uses three or five members");
    }
    groups.resize(config.size);
    members.resize(config.size);
    std::iota(members.begin(), members.end(), NodeId{1});
    for (std::size_t index = 0; index < config.size; ++index) {
      hosts.push_back(std::make_unique<Host>(*this, random.Next()));
      if (!config.initial.empty()) {
        if (config.initial.size() != config.size) {
          throw std::invalid_argument("one initial state required per member");
        }
        hosts.back()->storage = MemoryStorage(config.initial.at(index));
      }
    }
    for (const auto id : members) {
      Boot(id);
    }
  }

  VirtualClock clock;
  SeededRandom random;
  NetworkFaults faults;
  std::uint64_t vote_crash_percent{};
  std::uint64_t candidate_crash_percent{};
  std::uint64_t append_crash_percent{};
  ElectionSafety safety;
  ReplicationSafety replication;
  std::vector<NodeId> members;
  std::vector<std::uint64_t> groups;
  std::vector<std::unique_ptr<Host>> hosts;
  std::map<std::pair<TimePoint, std::uint64_t>, Message> queue;
  SimStats stats;
  std::uint64_t sequence{};
  std::uint64_t digest{14695981039346656037ULL};
  bool recording;
  bool manual_delivery;
  std::size_t max_append_entries;
  std::function<std::unique_ptr<StateMachine>()> machine_factory;
  bool unsafe_reads;
  LogIndex snapshot_threshold;
  std::size_t snapshot_chunk_bytes;
  // Set while an InstallSnapshot is being delivered, to tell installs from compactions.
  bool installing{};
  // First response observed at each committed index; every replica must agree.
  std::map<LogIndex, Applied> responses;
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
      digest ^= byte;
      digest *= 1099511628211ULL;
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
      auto& host = At(member);
      const auto state = host.storage.hard_state();
      const auto role = host.node ? host.node->role() : Role::Follower;
      safety.Observe(member, role, state);
      if (host.node && host.node->hard_state() != state) {
        throw std::logic_error("node hard state differs from durable storage");
      }
      if (host.checked_revision != host.storage.revision()) {
        const auto previous_base = host.checked_log.base_index();
        host.checked_log = RaftLog(host.storage.durable());
        const auto& durable = host.storage.durable();
        if (host.checked_log.base_index() != previous_base && durable.snapshot.has_value()) {
          replication.ObserveSnapshot(*durable.snapshot);
          ++(installing ? stats.snapshot_installs : stats.snapshots_taken);
        }
        stats.max_retained_entries =
            std::max<std::uint64_t>(stats.max_retained_entries, host.checked_log.entries().size());
        replication.ObserveLog(member, host.checked_log);
        host.checked_revision = host.storage.revision();
        for (const auto& entry : host.checked_log.entries()) {
          Hash(entry.index);
          Hash(entry.term);
          Hash(static_cast<std::uint64_t>(entry.kind));
          for (const auto byte : entry.command) {
            Hash(byte);
          }
        }
      }
      ++stats.log_checks;
      if (host.node) {
        const auto& log = host.node->log();
        if (!std::ranges::equal(log.entries(), host.checked_log.entries())) {
          throw std::logic_error("node log differs from durable storage");
        }
        if (role == Role::Leader) {
          replication.ObserveLeader(member, state.term, log);
        }
        if (host.node->commit_index() < host.applied) {
          throw std::logic_error("commit index moved backwards within a node lifetime");
        }
        if (host.node->commit_index() != host.applied) {
          replication.ObserveCommit(member, log, host.node->commit_index());
          host.applied = host.node->commit_index();
        }
        Hash(host.node->commit_index());
      }
      ++stats.application_checks;
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
      if (due - clock.Now() > 30) {
        ++stats.long_delay_packets;
      }
      Hash(due);
      queue.emplace(std::pair{due, sequence++}, message);
    }
  }

  void Boot(NodeId id) {
    auto& host = At(id);
    if (host.node) {
      return;
    }
    host.applied = 0;
    ++host.epoch;
    host.machine = machine_factory ? machine_factory() : nullptr;
    host.node =
        std::make_unique<RaftNode>(ElectionConfig{.id = id,
                                                  .members = members,
                                                  .max_append_entries = max_append_entries,
                                                  .snapshot_threshold = snapshot_threshold,
                                                  .snapshot_chunk_bytes = snapshot_chunk_bytes,
                                                  .unsafe_local_reads = unsafe_reads},
                                   host.link, clock, host.storage, host.random, host.machine.get());
    host.checked_applied = host.node->last_applied();
    // Restart from a snapshot must reproduce exactly the state that was saved.
    if (const auto& snapshot = host.storage.durable().snapshot; snapshot && host.machine) {
      ++stats.restore_checks;
      if (host.machine->Snapshot() != snapshot->state) {
        throw std::logic_error("restored state machine differs from its durable snapshot");
      }
    }
    ++stats.restarts;
    Check("restart", id);
  }

  // Responses leave the node as soon as they are produced, like a reply already sent.
  void Collect(Host& host) {
    if (!host.node) {
      return;
    }
    // Snapshot plus replayed log must reach the same state other replicas saved there.
    if (const auto applied = host.node->last_applied();
        host.machine && applied != host.checked_applied) {
      host.checked_applied = applied;
      if (const auto* snapshot = replication.SnapshotAt(applied)) {
        ++stats.restore_checks;
        if (host.machine->Snapshot() != snapshot->state) {
          throw std::logic_error("replayed state differs from a snapshot of the same index");
        }
      }
    }
    for (auto& applied : host.node->TakeApplied()) {
      ++stats.applied_commands;
      const auto [found, inserted] = responses.emplace(applied.index, applied);
      if (!inserted && found->second != applied) {
        throw std::logic_error("State Machine Safety: replicas produced different responses");
      }
      Hash(applied.index);
      for (const auto byte : applied.response) {
        Hash(byte);
      }
      host.applied_out.push_back({.epoch = host.epoch, .applied = std::move(applied)});
    }
    for (auto& read : host.node->TakeReads()) {
      ++(read.ok ? stats.reads_served : stats.reads_failed);
      Hash(read.id);
      Hash(read.ok ? 1 : 0);
      host.reads_out.push_back({.epoch = host.epoch, .read = std::move(read)});
    }
  }

  void Crash(NodeId id) {
    auto& host = At(id);
    Collect(host);
    host.node.reset();
    host.machine.reset();
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
      Collect(host);
    } catch (const PowerLoss&) {
      Crash(id);
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
    while (!manual_delivery && !queue.empty() && queue.begin()->first.first <= clock.Now()) {
      Deliver(queue.begin());
    }
  }

  void Deliver(const decltype(queue)::iterator& position) {
    const auto packet = queue.extract(position);
    const auto& message = packet.mapped();
    HashMessage(message);
    if (At(message.to).node && Connected(message)) {
      ++stats.delivered;
      const bool install = std::holds_alternative<InstallSnapshot>(message.body);
      if (install) {
        ++stats.snapshot_chunks;
      }
      if (const auto* vote = std::get_if<VoteResponse>(&message.body);
          vote && vote->granted && message.term < At(message.to).node->hard_state().term) {
        ++stats.stale_vote_deliveries;
      }
      installing = install;
      Run(message.to, [&message](RaftNode& node) { node.Step(message); });
      Check("deliver", message.to);
      installing = false;
    } else {
      ++stats.dropped;
      Check("drop", message.to);
    }
  }
};

Cluster::Cluster(const ClusterConfig& config) : impl_(std::make_unique<Impl>(config)) {}
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
void Cluster::SetVoteCrashPercent(std::uint64_t percent) {
  if (percent > 100) {
    throw std::invalid_argument("invalid vote crash percentage");
  }
  impl_->vote_crash_percent = percent;
  impl_->Hash(percent);
  impl_->Check("vote-crash-fault");
}
void Cluster::SetReplicationCrashPercent(std::uint64_t candidate, std::uint64_t append_reply) {
  if (candidate > 100 || append_reply > 100) {
    throw std::invalid_argument("invalid replication crash percentage");
  }
  impl_->candidate_crash_percent = candidate;
  impl_->append_crash_percent = append_reply;
  impl_->Hash(candidate);
  impl_->Hash(append_reply);
  impl_->Check("replication-crash-fault");
}
auto Cluster::Propose(NodeId id, Bytes command) -> std::optional<LogIndex> {
  std::optional<LogIndex> index;
  ++impl_->stats.proposals;
  for (const auto byte : command) {
    impl_->Hash(byte);
  }
  impl_->Run(id, [&](RaftNode& node) { index = node.Propose(std::move(command)); });
  impl_->Check("propose", id);
  return index;
}
auto Cluster::Submit(NodeId id, Bytes command) -> std::optional<LogPosition> {
  std::optional<LogPosition> position;
  const auto epoch = impl_->At(id).epoch;
  for (const auto byte : command) {
    impl_->Hash(byte);
  }
  impl_->Run(id, [&](RaftNode& node) {
    if (const auto index = node.Propose(std::move(command))) {
      position = LogPosition{.index = *index, .term = node.hard_state().term};
    }
  });
  // A crash inside the call means the client never heard back.
  if (impl_->At(id).epoch != epoch || !impl_->At(id).node) {
    position.reset();
  }
  ++impl_->stats.proposals;
  impl_->Check(position ? "submit" : "submit-rejected", id);
  return position;
}
auto Cluster::Read(NodeId id, ClientRead read) -> bool {
  bool accepted = false;
  const auto epoch = impl_->At(id).epoch;
  impl_->Hash(read.id);
  impl_->Run(id, [&](RaftNode& node) { accepted = node.Read(read.id, std::move(read.query)); });
  accepted = accepted && impl_->At(id).epoch == epoch && impl_->At(id).node;
  impl_->Check(accepted ? "read" : "read-rejected", id);
  return accepted;
}
auto Cluster::TakeApplied(NodeId id) -> std::vector<NodeApplied> {
  return std::exchange(impl_->At(id).applied_out, {});
}
auto Cluster::TakeReads(NodeId id) -> std::vector<NodeRead> {
  return std::exchange(impl_->At(id).reads_out, {});
}
auto Cluster::Epoch(NodeId id) const -> std::uint64_t { return impl_->At(id).epoch; }
auto Cluster::LeaderHint(NodeId id) const -> std::optional<NodeId> {
  const auto& node = impl_->At(id).node;
  return node ? node->leader_hint() : std::nullopt;
}
auto Cluster::Size() const -> std::size_t { return impl_->members.size(); }
auto Cluster::MachineSnapshot(NodeId id) const -> Bytes {
  const auto& host = impl_->At(id);
  return host.node && host.machine ? host.machine->Snapshot() : Bytes{};
}
auto Cluster::LastApplied(NodeId id) const -> LogIndex {
  const auto& node = impl_->At(id).node;
  return node ? node->last_applied() : 0;
}
auto Cluster::Log(NodeId id) const -> PersistentState { return impl_->At(id).storage.Load(); }
auto Cluster::CommitIndex(NodeId id) const -> LogIndex {
  const auto& node = impl_->At(id).node;
  return node ? node->commit_index() : 0;
}
auto Cluster::Committed() const -> const std::vector<LogEntry>& {
  return impl_->replication.committed();
}
auto Cluster::Pending() const -> std::vector<std::pair<std::uint64_t, Message>> {
  std::vector<std::pair<std::uint64_t, Message>> packets;
  for (const auto& [key, message] : impl_->queue) {
    packets.emplace_back(key.second, message);
  }
  return packets;
}
void Cluster::Deliver(std::uint64_t packet) {
  if (!impl_->manual_delivery) {
    throw std::logic_error("directed delivery requires manual mode");
  }
  const auto found = std::ranges::find_if(
      impl_->queue, [packet](const auto& item) { return item.first.second == packet; });
  if (found == impl_->queue.end()) {
    throw std::out_of_range("unknown queued packet");
  }
  impl_->Deliver(found);
}
void Cluster::Tick(NodeId id) {
  if (!impl_->manual_delivery) {
    throw std::logic_error("directed tick requires manual mode");
  }
  impl_->clock.Advance();
  impl_->Check("clock");
  impl_->Run(id, [](RaftNode& node) { node.Tick(); });
  impl_->Check("tick", id);
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
