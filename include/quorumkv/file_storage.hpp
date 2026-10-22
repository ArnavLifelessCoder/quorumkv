#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "quorumkv/interfaces.hpp"

namespace quorumkv {

namespace platform {
class AppendFile;
}

// Crash boundaries the storage tests can stop at.
enum class StoragePoint : std::uint8_t {
  TemporarySynced,  // compaction: new image written and synced, not yet published
  Replaced,         // compaction: new image published, journal not yet truncated
  JournalSynced,    // journal record appended and synced
};

inline constexpr std::size_t kDefaultJournalBytes = std::size_t{8} * 1024 * 1024;

// Single-owner durable storage: a checksummed base image (storage.bin) plus an append-only
// journal (journal.bin) of checksummed, sequence-numbered batch records.
//
// Persist appends one record and syncs it, so its cost does not grow with the log. Once the
// journal passes `journal_bytes`, or when a batch publishes a snapshot, the whole state is
// written as a new image (temporary file, sync, atomic rename) and the journal is truncated.
// Replay skips records the image already contains, so a crash between publishing an image and
// truncating the journal never applies a batch twice. A torn final record was never
// acknowledged and is dropped; damage to any earlier record is reported as corruption.
class FileStorage final : public Storage {
 public:
  explicit FileStorage(std::filesystem::path directory, std::function<void(StoragePoint)> hook = {},
                       std::size_t journal_bytes = kDefaultJournalBytes);
  ~FileStorage() override;
  FileStorage(const FileStorage&) = delete;
  auto operator=(const FileStorage&) -> FileStorage& = delete;
  FileStorage(FileStorage&&) = delete;
  auto operator=(FileStorage&&) -> FileStorage& = delete;

  auto Load() -> PersistentState override;
  void Persist(const StorageBatch& batch) override;

 private:
  void Compact(const PersistentState& state);

  std::filesystem::path directory_;
  std::function<void(StoragePoint)> hook_;
  std::size_t journal_limit_;
  PersistentState state_;
  std::unique_ptr<platform::AppendFile> journal_;
  std::uint64_t sequence_{};  // last durable batch
  std::uint64_t journal_size_{};
  std::uint64_t image_size_{};
  bool stopped_{};
};

}  // namespace quorumkv
