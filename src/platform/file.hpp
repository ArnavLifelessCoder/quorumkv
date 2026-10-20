#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace quorumkv::platform {

void WriteSynced(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
// Both paths must be in the same existing directory and filesystem.
void AtomicReplace(const std::filesystem::path& source, const std::filesystem::path& destination);

}  // namespace quorumkv::platform
