#include "quorumkv/kv_workload.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "quorumkv/kv.hpp"

namespace quorumkv::sim {
namespace {

constexpr std::uint64_t kTimeout = 30;
constexpr unsigned kActiveTicks = 300;
constexpr unsigned kQuiesceLimit = 1500;
const std::vector<std::string> kAppendKeys{"a0", "a1"};
const std::vector<std::string> kRegisterKeys{"r0", "r1"};

enum class State : std::uint8_t { Idle, Ready, Waiting };

struct Client {
  std::uint64_t id{};
  kv::ClientId identity;
  std::uint64_t sequence{};
  State state{State::Idle};
  std::size_t op{};
  // Logged requests keep one sequence across retries. ReadIndex gets carry no session.
  std::optional<kv::Request> request;
  std::string read_key;
  NodeId target{1};
  NodeId node{};
  std::uint64_t epoch{};
  std::optional<LogPosition> position;
  std::optional<std::uint64_t> read;
  std::uint64_t deadline{};
};

class Driver {
 public:
  explicit Driver(const KvScenarioConfig& config)
      : config_(config),
        cluster_(
            ClusterConfig{.seed = config.seed,
                          .size = config.seed % 2 == 0 ? 3U : 5U,
                          .record_trace = config.record_trace,
                          .max_append_entries = 1 + config.seed % 8,
                          .machine_factory = config.machine_factory
                                                 ? config.machine_factory
                                                 : [] { return std::make_unique<kv::Store>(); },
                          .unsafe_reads = config.unsafe_reads}),
        nemesis_(config.seed ^ 0x6b766e656d657369ULL),
        choice_(config.seed ^ 0x6b76636c69656e74ULL) {
    const auto count = 3 + config.seed % 3;
    for (std::uint64_t id = 1; id <= count; ++id) {
      Client client;
      client.id = id;
      client.identity = {.high = config.seed, .low = id};
      client.target = 1 + (id - 1) % cluster_.Size();
      clients_.push_back(std::move(client));
    }
  }

  auto Run() -> KvScenarioResult {
    const auto size = cluster_.Size();
    // Power loss right after a granted vote, a sent RequestVote, or an append acknowledgement.
    cluster_.SetVoteCrashPercent(5 + nemesis_.Next() % 11);
    cluster_.SetReplicationCrashPercent(1 + nemesis_.Next() % 4, 1 + nemesis_.Next() % 3);
    for (unsigned tick = 0; tick < kActiveTicks; ++tick) {
      Nemesis(tick);
      Step(true);
    }
    cluster_.Heal();
    cluster_.SetVoteCrashPercent(0);
    cluster_.SetReplicationCrashPercent(0, 0);
    cluster_.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
    for (NodeId id = 1; id <= size; ++id) {
      cluster_.FailNextSync(id, SyncFailure::None);
      cluster_.Restart(id);
    }
    Quiesce();
    // Final linearizable reads through the ordinary client path define the final values.
    std::map<std::string, std::string> final_values;
    for (const auto& key : kAppendKeys) {
      auto& client = clients_.front();
      Begin(client, OpKind::Get, key, {}, {});
      client.read_key = key;
      Quiesce();
      const auto& op = result_.history.at(client.op);
      final_values[key] = op.found ? op.value : std::string{};
    }
    CheckHistory(result_.history, final_values);
    result_.final_values.assign(final_values.begin(), final_values.end());
    result_.stats = cluster_.Stats();
    result_.digest = cluster_.Digest();
    result_.trace = cluster_.Trace();
    return std::move(result_);
  }

 private:
  void Nemesis(unsigned tick) {
    const auto size = cluster_.Size();
    if (tick % 40 == 0) {
      cluster_.SetFaults(
          {.drop_percent = nemesis_.Next() % 21,
           .duplicate_percent = nemesis_.Next() % 31,
           .max_delay = tick % 80 == 0 ? 31 + nemesis_.Next() % 30 : 1 + nemesis_.Next() % 3});
      std::vector<std::uint64_t> groups(size);
      for (auto& group : groups) {
        group = nemesis_.Next() % 2;
      }
      groups.front() = 0;
      groups.back() = 1;
      cluster_.Partition(groups);
    }
    if (tick % 40 == 20) {
      cluster_.Heal();
    }
    if (tick % 17 == 0) {
      cluster_.FailNextSync(1 + nemesis_.Next() % size, nemesis_.Next() % 2 == 0
                                                            ? SyncFailure::BeforeSync
                                                            : SyncFailure::AfterSync);
    }
    if (tick % 47 == 0) {
      const auto leaders = cluster_.Leaders();
      cluster_.Crash(leaders.empty() ? 1 + nemesis_.Next() % size : leaders.front());
    }
    if (tick % 9 == 8) {
      for (NodeId id = 1; id <= size; ++id) {
        cluster_.Restart(id);
      }
    }
  }

  void Quiesce() {
    for (unsigned tick = 0;; ++tick) {
      if (std::ranges::all_of(clients_, [](const auto& c) { return c.state == State::Idle; })) {
        return;
      }
      if (tick == kQuiesceLimit) {
        throw std::runtime_error("healed cluster did not finish client operations");
      }
      Step(false);
      ++result_.quiesce_ticks;
    }
  }

  void Step(bool issue) {
    cluster_.Advance();
    ++ticks_;
    for (NodeId id = 1; id <= cluster_.Size(); ++id) {
      for (auto& output : cluster_.TakeApplied(id)) {
        Deliver(id, output);
      }
      for (auto& output : cluster_.TakeReads(id)) {
        Deliver(id, output);
      }
    }
    const auto now = ticks_;
    for (auto& client : clients_) {
      if (client.state == State::Idle && issue) {
        Choose(client);
      }
      if (client.state == State::Waiting &&
          (cluster_.Epoch(client.node) != client.epoch || now >= client.deadline)) {
        ++result_.timeouts;
        Retry(client);
      }
      if (client.state == State::Ready) {
        Attempt(client, now);
      }
    }
  }

  void Choose(Client& client) {
    const auto roll = choice_.Next() % 100;
    const auto pick = [&](const std::vector<std::string>& keys) {
      return keys.at(static_cast<std::size_t>(choice_.Next() % keys.size()));
    };
    const auto token = "c" + std::to_string(client.id) + "s" + std::to_string(client.sequence + 1);
    if (roll < 35) {
      Begin(client, OpKind::Append, pick(kAppendKeys), token + ";", {});
    } else if (roll < 50) {
      Begin(client, OpKind::Put, pick(kRegisterKeys), token, {});
    } else if (roll < 60) {
      const auto key = pick(kRegisterKeys);
      const auto seen = last_seen_.find(key);
      Begin(client, OpKind::Cas, key, token, seen == last_seen_.end() ? "" : seen->second);
    } else {
      const auto& keys = choice_.Next() % 2 == 0 ? kAppendKeys : kRegisterKeys;
      const auto key = pick(keys);
      if (roll < 90) {
        Begin(client, OpKind::Get, key, {}, {});
        client.read_key = key;
      } else {
        // Some gets go through the log, exercising session caching of read results.
        ++result_.logged_reads;
        Begin(client, OpKind::Get, key, {}, {}, true);
      }
    }
  }

  void Begin(Client& client, OpKind kind, const std::string& key, std::string argument,
             std::string expected, bool logged_get = false) {
    client.op = result_.history.size();
    HistoryOp op;
    op.client = client.id;
    op.kind = kind;
    op.key = key;
    op.argument = argument;
    op.expected = expected;
    op.invoke = ++events_;
    result_.history.push_back(std::move(op));
    client.request.reset();
    client.read_key.clear();
    if (kind != OpKind::Get || logged_get) {
      kv::Operation operation;
      switch (kind) {
        case OpKind::Append:
          operation = kv::Append{.key = key, .suffix = std::move(argument)};
          break;
        case OpKind::Put:
          operation = kv::Put{.key = key, .value = std::move(argument)};
          break;
        case OpKind::Cas:
          operation = kv::Cas{
              .key = key, .expected = std::move(expected), .replacement = std::move(argument)};
          break;
        case OpKind::Get:
          operation = kv::Get{.key = key};
          break;
      }
      client.request = kv::Request{
          .client = client.identity, .sequence = ++client.sequence, .operation = operation};
    }
    client.state = State::Ready;
  }

  // The outcome of the previous attempt is unknown, so the same request is sent again.
  void Retry(Client& client) {
    ++result_.retries;
    client.state = State::Ready;
    client.position.reset();
    client.read.reset();
    client.target = client.target % cluster_.Size() + 1;
  }

  void Attempt(Client& client, std::uint64_t now) {
    const auto target = client.target;
    const auto epoch = cluster_.Epoch(target);
    bool accepted = false;
    if (client.request) {
      if (const auto position = cluster_.Submit(target, kv::EncodeRequest(*client.request))) {
        client.position = position;
        accepted = true;
      }
    } else {
      const auto read = ++read_ids_;
      reads_[read] = static_cast<std::size_t>(&client - clients_.data());
      if (cluster_.Read(target, {.id = read, .query = kv::EncodeRead(client.read_key)})) {
        client.read = read;
        accepted = true;
      } else {
        reads_.erase(read);
      }
    }
    if (accepted) {
      client.state = State::Waiting;
      client.node = target;
      client.epoch = epoch;
      client.deadline = now + kTimeout;
      return;
    }
    ++result_.not_leader;
    const auto hint = cluster_.LeaderHint(target);
    client.target = hint && *hint != target ? *hint : target % cluster_.Size() + 1;
  }

  void Deliver(NodeId id, const NodeApplied& output) {
    for (auto& client : clients_) {
      if (client.state != State::Waiting || !client.position || client.node != id ||
          client.epoch != output.epoch || client.position->index != output.applied.index) {
        continue;
      }
      if (client.position->term != output.applied.term) {
        // Another leader's entry replaced ours at this index.
        Retry(client);
      } else {
        Complete(client, output.applied.response);
      }
    }
  }

  void Deliver(NodeId id, const NodeRead& output) {
    const auto found = reads_.find(output.read.id);
    if (found == reads_.end()) {
      return;
    }
    auto& client = clients_.at(found->second);
    reads_.erase(found);
    if (client.state != State::Waiting || client.read != output.read.id || client.node != id ||
        client.epoch != output.epoch) {
      return;
    }
    if (output.read.ok) {
      Complete(client, output.read.response);
    } else {
      Retry(client);
    }
  }

  void Complete(Client& client, const Bytes& bytes) {
    auto& op = result_.history.at(client.op);
    const auto response = kv::DecodeResponse(bytes);
    std::visit(
        [&](const auto& result) {
          using T = std::decay_t<decltype(result)>;
          if constexpr (std::is_same_v<T, kv::WriteOk>) {
            if (op.kind != OpKind::Append && op.kind != OpKind::Put) {
              throw std::logic_error("write acknowledgement for a non-write");
            }
          } else if constexpr (std::is_same_v<T, kv::Value>) {
            if (op.kind != OpKind::Get) {
              throw std::logic_error("value returned for a non-read");
            }
            op.found = result.found;
            op.value = result.value;
            if (result.found) {
              last_seen_[op.key] = result.value;
            }
          } else if constexpr (std::is_same_v<T, kv::CasResult>) {
            if (op.kind != OpKind::Cas) {
              throw std::logic_error("CAS result for a non-CAS");
            }
            op.exchanged = result.exchanged;
          } else {
            // A client only ever retries its latest sequence, and every command decodes.
            throw std::logic_error("session returned a stale or invalid response");
          }
        },
        response);
    op.complete = ++events_;
    client.state = State::Idle;
    client.position.reset();
    client.read.reset();
  }

  KvScenarioConfig config_;
  Cluster cluster_;
  SeededRandom nemesis_;
  SeededRandom choice_;
  std::vector<Client> clients_;
  std::map<std::uint64_t, std::size_t> reads_;
  std::map<std::string, std::string> last_seen_;
  std::uint64_t ticks_{};
  std::uint64_t events_{};
  std::uint64_t read_ids_{};
  KvScenarioResult result_;
};

}  // namespace

auto RunKvScenario(const KvScenarioConfig& config) -> KvScenarioResult {
  Driver driver(config);
  return driver.Run();
}

}  // namespace quorumkv::sim
