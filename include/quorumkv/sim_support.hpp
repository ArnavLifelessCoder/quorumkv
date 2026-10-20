#pragma once

#include <map>
#include <utility>

#include "quorumkv/raft_node.hpp"

namespace quorumkv::sim {

class SeededRandom final : public Random {
 public:
  explicit SeededRandom(std::uint64_t seed) : state_(seed) {}
  auto Next() -> std::uint64_t override;

 private:
  std::uint64_t state_;
};

class VirtualClock final : public Clock {
 public:
  auto Now() -> TimePoint override { return now_; }
  void Advance();

 private:
  TimePoint now_{};
};

enum class SyncFailure : std::uint8_t { None, BeforeSync, AfterSync };

// M2 models atomic hard-state sync. Log/snapshot writes are deliberately rejected until M3.
class MemoryStorage final : public Storage {
 public:
  explicit MemoryStorage(PersistentState initial = {}) : durable_(std::move(initial)) {}
  auto Load() -> PersistentState override { return durable_; }
  void Persist(const StorageBatch& batch) override;
  void FailNext(SyncFailure failure) { failure_ = failure; }
  void Crash();
  [[nodiscard]] auto hard_state() const -> HardState { return durable_.hard_state; }

 private:
  PersistentState durable_;
  std::optional<HardState> pending_;
  SyncFailure failure_{SyncFailure::None};
};

// Retains history across crashes, restarts, and step-downs.
class ElectionSafety {
 public:
  void Observe(NodeId id, Role role, HardState state);
  [[nodiscard]] auto leaders() const -> const std::map<Term, NodeId>& { return leaders_; }

 private:
  std::map<Term, NodeId> leaders_;
  std::map<NodeId, HardState> previous_;
};

}  // namespace quorumkv::sim
