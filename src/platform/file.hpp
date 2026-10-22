#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace quorumkv::platform {

void WriteSynced(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
// Both paths must be in the same existing directory and filesystem.
void AtomicReplace(const std::filesystem::path& source, const std::filesystem::path& destination);
// Makes a newly created or removed directory entry durable. A no-op where the filesystem
// already does so (Windows).
void SyncDirectory(const std::filesystem::path& directory);

// An append-only file kept open across writes. Append buffers nothing in user space;
// Sync returns once every appended byte is durable.
class AppendFile {
 public:
  explicit AppendFile(const std::filesystem::path& path);
  ~AppendFile();
  AppendFile(const AppendFile&) = delete;
  auto operator=(const AppendFile&) -> AppendFile& = delete;
  AppendFile(AppendFile&&) = delete;
  auto operator=(AppendFile&&) -> AppendFile& = delete;
  void Append(std::span<const std::uint8_t> bytes);
  void Sync();
  // Shrinks the file to `size` bytes and syncs.
  void Truncate(std::uint64_t size);

 private:
  struct Native;
  std::unique_ptr<Native> native_;
};

}  // namespace quorumkv::platform
