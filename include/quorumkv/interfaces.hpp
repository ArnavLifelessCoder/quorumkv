#pragma once

#include <cstdint>
#include <span>

#include "quorumkv/types.hpp"

namespace quorumkv {

struct Transport {
  virtual ~Transport() = default;
  // Enqueue a copy; no synchronous callback into the sender. to must equal message.to.
  virtual void Send(NodeId to, const Message& message) = 0;
};

struct Clock {
  virtual ~Clock() = default;
  // Monotonic logical ticks. The adapter defines their duration; no wall-clock epoch.
  virtual auto Now() -> TimePoint = 0;
};

struct Random {
  virtual ~Random() = default;
  virtual auto Next() -> std::uint64_t = 0;
};

struct Storage {
  virtual ~Storage() = default;
  virtual auto Load() -> PersistentState = 0;
  // Atomic durable batch: truncate suffix, append, publish snapshot and discard its prefix,
  // update hard state. Return only after sync; throw on failure and stop the node.
  virtual void Persist(const StorageBatch& batch) = 0;
};

struct StateMachine {
  virtual ~StateMachine() = default;
  // Only committed commands reach Apply. Command/response encoding belongs to the KV layer.
  virtual auto Apply(std::span<const std::uint8_t> command) -> Bytes = 0;
  // The caller must establish ReadIndex safety before reading.
  [[nodiscard]] virtual auto Read(std::span<const std::uint8_t> query) const -> Bytes = 0;
  // Snapshot/Restore include application data and client session responses.
  [[nodiscard]] virtual auto Snapshot() const -> Bytes = 0;
  virtual void Restore(std::span<const std::uint8_t> snapshot) = 0;
};

}  // namespace quorumkv
