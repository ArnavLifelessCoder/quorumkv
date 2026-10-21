#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "quorumkv/kv_history.hpp"
#include "quorumkv/sim_cluster.hpp"

namespace quorumkv::sim {

struct KvScenarioConfig {
  std::uint64_t seed{};
  bool record_trace{};
  // Deliberately broken modes used to prove the checkers find real bugs.
  bool unsafe_reads{};
  std::function<std::unique_ptr<StateMachine>()> machine_factory{};
};

struct KvScenarioResult {
  std::vector<HistoryOp> history;
  // Final value of each append key, ordered by key.
  std::vector<std::pair<std::string, std::string>> final_values;
  SimStats stats;
  std::uint64_t digest{};
  std::uint64_t retries{};
  std::uint64_t timeouts{};
  std::uint64_t not_leader{};
  std::uint64_t logged_reads{};
  std::uint64_t quiesce_ticks{};
  std::string trace;
};

// Runs clients against a 3- or 5-node simulated cluster under partitions, crashes, sync
// failures, long delays, and duplicate retries, then checks the recorded history with
// CheckHistory. Throws std::logic_error on a safety violation and std::runtime_error when
// the healed cluster fails to finish outstanding operations.
auto RunKvScenario(const KvScenarioConfig& config) -> KvScenarioResult;

}  // namespace quorumkv::sim
