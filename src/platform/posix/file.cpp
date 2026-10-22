#include "../file.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <system_error>

namespace quorumkv::platform {
namespace {

class File {
 public:
  File(const std::filesystem::path& path, int flags)
      : descriptor_(open(path.c_str(), flags, 0600)) {
    if (descriptor_ < 0) {
      throw std::system_error(errno, std::generic_category(), "open storage path");
    }
  }
  ~File() { close(descriptor_); }
  File(const File&) = delete;
  auto operator=(const File&) -> File& = delete;
  [[nodiscard]] auto get() const -> int { return descriptor_; }
  void Sync() const {
    int result{};
    do {
      result = fsync(descriptor_);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
      throw std::system_error(errno, std::generic_category(), "sync storage path");
    }
  }

 private:
  int descriptor_;
};

}  // namespace

void WriteSynced(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  const File file(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
  while (!bytes.empty()) {
    const auto count = std::min(bytes.size(), std::size_t{1024} * 1024);
    const auto written = write(file.get(), bytes.data(), count);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::system_error(written == 0 ? EIO : errno, std::generic_category(),
                              "write storage staging file");
    }
    bytes = bytes.subspan(static_cast<std::size_t>(written));
  }
  file.Sync();
}

void AtomicReplace(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (rename(source.c_str(), destination.c_str()) != 0) {
    throw std::system_error(errno, std::generic_category(), "replace durable storage image");
  }
  const File directory(destination.parent_path(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  directory.Sync();
}

void SyncDirectory(const std::filesystem::path& directory) {
  const File handle(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  handle.Sync();
}

struct AppendFile::Native {
  int descriptor;
};

AppendFile::AppendFile(const std::filesystem::path& path)
    : native_(std::make_unique<Native>(
          Native{open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600)})) {
  if (native_->descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open storage journal");
  }
}

AppendFile::~AppendFile() { close(native_->descriptor); }

void AppendFile::Append(std::span<const std::uint8_t> bytes) {
  while (!bytes.empty()) {
    const auto count = std::min(bytes.size(), std::size_t{1024} * 1024);
    const auto written = write(native_->descriptor, bytes.data(), count);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::system_error(written == 0 ? EIO : errno, std::generic_category(),
                              "append storage journal");
    }
    bytes = bytes.subspan(static_cast<std::size_t>(written));
  }
}

void AppendFile::Sync() {
  int result{};
  do {
    result = fsync(native_->descriptor);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    throw std::system_error(errno, std::generic_category(), "sync storage journal");
  }
}

void AppendFile::Truncate(std::uint64_t size) {
  if (ftruncate(native_->descriptor, static_cast<off_t>(size)) != 0) {
    throw std::system_error(errno, std::generic_category(), "truncate storage journal");
  }
  Sync();
}

}  // namespace quorumkv::platform
