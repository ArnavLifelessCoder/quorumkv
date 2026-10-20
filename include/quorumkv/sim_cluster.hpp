#pragma once

#include <memory>
#include <string>

#include "quorumkv/sim_support.hpp"

namespace quorumkv::sim {

struct NetworkFaults {
  std::uint64_t drop_percent{10};
  std::uint64_t duplicate_percent{10};
  TimePoint max_delay{3};
};

struct ClusterConfig {
  std::uint64_t seed{};
  std::size_t size{3};
  bool record_trace{};
};

struct SimStats {
  std::uint64_t steps{};
  std::uint64_t safety_checks{};
  std::uint64_t delivered{};
  std::uint64_t dropped{};
  std::uint64_t duplicated{};
  std::uint64_t partitions{};
  std::uint64_t crashes{};
  std::uint64_t restarts{};
  std::uint64_t sync_failures{};
  std::uint64_t vote_grant_crashes{};
  std::uint64_t long_delay_packets{};
  std::uint64_t stale_vote_deliveries{};
  auto operator==(const SimStats&) const -> bool = default;
};

class Cluster {
 public:
  explicit Cluster(ClusterConfig config);
  ~Cluster();
  Cluster(const Cluster&) = delete;
  auto operator=(const Cluster&) -> Cluster& = delete;
  Cluster(Cluster&&) = delete;
  auto operator=(Cluster&&) -> Cluster& = delete;
  void Advance();
  void Crash(NodeId id);
  void Restart(NodeId id);
  void FailNextSync(NodeId id, SyncFailure failure);
  // Different labels cannot communicate; packets are checked at send and delivery.
  void Partition(const std::vector<std::uint64_t>& groups);
  void Heal();
  void SetFaults(NetworkFaults faults);
  // Abrupt power loss at the Transport::Send boundary, after a granted reply is queued.
  void SetVoteCrashPercent(std::uint64_t percent);
  [[nodiscard]] auto Leaders() const -> std::vector<NodeId>;
  [[nodiscard]] auto Alive(NodeId id) const -> bool;
  [[nodiscard]] auto State(NodeId id) const -> HardState;
  [[nodiscard]] auto Stats() const -> SimStats;
  [[nodiscard]] auto Digest() const -> std::uint64_t;
  [[nodiscard]] auto Trace() const -> const std::string&;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace quorumkv::sim
