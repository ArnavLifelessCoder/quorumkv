#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../file.hpp"

#include <windows.h>

#include <algorithm>
#include <system_error>

namespace quorumkv::platform {
namespace {

class File {
 public:
  explicit File(const std::filesystem::path& path)
      : handle_(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)) {
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                              "create storage staging file");
    }
  }
  ~File() { CloseHandle(handle_); }
  File(const File&) = delete;
  auto operator=(const File&) -> File& = delete;
  [[nodiscard]] auto get() const -> HANDLE { return handle_; }

 private:
  HANDLE handle_;
};

}  // namespace

void WriteSynced(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  const File file(path);
  while (!bytes.empty()) {
    const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
    DWORD written{};
    if (!WriteFile(file.get(), bytes.data(), count, &written, nullptr) || written == 0) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                              "write storage staging file");
    }
    bytes = bytes.subspan(written);
  }
  if (!FlushFileBuffers(file.get())) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            "sync storage staging file");
  }
}

void AtomicReplace(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (!MoveFileExW(source.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            "replace durable storage image");
  }
}

}  // namespace quorumkv::platform
