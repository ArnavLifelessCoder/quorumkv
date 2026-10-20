#pragma once

#include <filesystem>
#include <functional>

#include "quorumkv/interfaces.hpp"

namespace quorumkv {

enum class StoragePoint : std::uint8_t { TemporarySynced, Replaced };

// Single-owner storage. Each batch publishes a checksummed complete image.
// The hook provides deterministic crash boundaries for storage tests.
class FileStorage final : public Storage {
 public:
  explicit FileStorage(std::filesystem::path directory,
                       std::function<void(StoragePoint)> hook = {});
  auto Load() -> PersistentState override;
  void Persist(const StorageBatch& batch) override;

 private:
  std::filesystem::path directory_;
  std::function<void(StoragePoint)> hook_;
  PersistentState state_;
  bool stopped_{};
};

}  // namespace quorumkv
