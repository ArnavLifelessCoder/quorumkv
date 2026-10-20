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
  virtual TimePoint Now() = 0;
};

struct Random {
  virtual ~Random() = default;
  virtual std::uint64_t Next() = 0;
};

struct Storage {
  virtual ~Storage() = default;
  virtual PersistentState Load() = 0;
  // Atomic durable batch: truncate suffix, append, publish snapshot and discard its prefix,
  // update hard state. Return only after sync; throw on failure and stop the node.
  virtual void Persist(const StorageBatch& batch) = 0;
};

struct StateMachine {
  virtual ~StateMachine() = default;
  // Only committed commands reach Apply. Command/response encoding belongs to the KV layer.
  virtual Bytes Apply(std::span<const std::uint8_t> command) = 0;
  // The caller must establish ReadIndex safety before reading.
  virtual Bytes Read(std::span<const std::uint8_t> query) const = 0;
  // Snapshot/Restore include application data and client session responses.
  virtual Bytes Snapshot() const = 0;
  virtual void Restore(std::span<const std::uint8_t> snapshot) = 0;
};

}  // namespace quorumkv
