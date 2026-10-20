#include <limits>
#include <stdexcept>
#include <utility>

#include "quorumkv/sim_support.hpp"

namespace quorumkv::sim {

auto SeededRandom::Next() -> std::uint64_t {
  auto value = (state_ += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

void VirtualClock::Advance() {
  if (now_ == std::numeric_limits<TimePoint>::max()) {
    throw std::overflow_error("virtual time exhausted");
  }
  ++now_;
}

void MemoryStorage::Persist(const StorageBatch& batch) {
  if (batch.truncate_from || !batch.append.empty() || batch.snapshot) {
    throw std::invalid_argument("M2 simulated storage only writes hard state");
  }
  const auto failure = std::exchange(failure_, SyncFailure::None);
  pending_ = batch.hard_state;
  if (failure == SyncFailure::BeforeSync) {
    throw std::runtime_error("simulated crash before sync");
  }
  if (pending_) {
    durable_.hard_state = *pending_;
  }
  pending_.reset();
  if (failure == SyncFailure::AfterSync) {
    throw std::runtime_error("simulated crash after sync");
  }
}

void MemoryStorage::Crash() {
  pending_.reset();
  failure_ = SyncFailure::None;
}

void ElectionSafety::Observe(NodeId id, Role role, HardState state) {
  if (const auto old = previous_.find(id); old != previous_.end()) {
    if (state.term < old->second.term || (state.term == old->second.term && old->second.voted_for &&
                                          state.voted_for != old->second.voted_for)) {
      throw std::logic_error("term regressed or durable vote changed within a term");
    }
  }
  previous_[id] = state;
  if (role == Role::Leader) {
    const auto [where, inserted] = leaders_.emplace(state.term, id);
    if (!inserted && where->second != id) {
      throw std::logic_error("two different leaders elected in the same term");
    }
  }
}

}  // namespace quorumkv::sim
