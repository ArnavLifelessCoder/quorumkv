#include "quorumkv/file_storage.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <utility>

#include "../platform/file.hpp"
#include "quorumkv/codec.hpp"
#include "quorumkv/log.hpp"

namespace quorumkv {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'Q', 'K', 'V', 'L', 'O', 'G', '0', '1'};
constexpr std::size_t kMaxImageBytes = std::size_t{64} * 1024 * 1024;

auto Checksum(std::span<const std::uint8_t> bytes) -> std::uint32_t {
  std::uint32_t crc = 0xffffffffU;
  for (const auto byte : bytes) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xedb88320U : 0U);
    }
  }
  return ~crc;
}

template <unsigned Width>
void Integer(Bytes& bytes, std::uint64_t value) {
  for (unsigned index = Width; index > 0; --index) {
    bytes.push_back(static_cast<std::uint8_t>(value >> (8 * (index - 1))));
  }
}

auto Integer(std::span<const std::uint8_t>& bytes, unsigned width) -> std::uint64_t {
  if (bytes.size() < width) {
    throw std::runtime_error("truncated storage image");
  }
  std::uint64_t value{};
  for (unsigned index = 0; index < width; ++index) {
    value = (value << 8) | bytes[index];
  }
  bytes = bytes.subspan(width);
  return value;
}

auto Serialize(const PersistentState& state) -> Bytes {
  if (state.snapshot) {
    throw std::invalid_argument("file snapshots require M5");
  }
  Bytes bytes(kMagic.begin(), kMagic.end());
  Integer<8>(bytes, state.hard_state.term);
  Integer<1>(bytes, state.hard_state.voted_for.has_value() ? 1 : 0);
  Integer<8>(bytes, state.hard_state.voted_for.value_or(0));
  Integer<8>(bytes, state.entries.size());
  for (const auto& entry : state.entries) {
    const auto frame =
        Encode({.term = state.hard_state.term, .body = AppendEntries{.entries = {entry}}});
    if (frame.size() > kMaxImageBytes - bytes.size() - 4) {
      throw std::length_error("storage image exceeds 64 MiB limit");
    }
    bytes.insert(bytes.end(), frame.begin(), frame.end());
  }
  Integer<4>(bytes, Checksum(bytes));
  return bytes;
}

auto Deserialize(std::span<const std::uint8_t> bytes) -> PersistentState {
  if (bytes.size() < 37 || !std::ranges::equal(bytes.first(8), kMagic)) {
    throw std::runtime_error("invalid storage image header");
  }
  auto checksum_bytes = bytes.last(4);
  if (Integer(checksum_bytes, 4) != Checksum(bytes.first(bytes.size() - 4))) {
    throw std::runtime_error("storage checksum mismatch");
  }
  bytes = bytes.subspan(8, bytes.size() - 12);
  PersistentState state;
  state.hard_state.term = Integer(bytes, 8);
  const auto has_vote = Integer(bytes, 1);
  const auto vote = Integer(bytes, 8);
  if (has_vote > 1 || (has_vote == 0 && vote != 0)) {
    throw std::runtime_error("invalid stored vote");
  }
  if (has_vote != 0) {
    state.hard_state.voted_for = vote;
  }
  const auto count = Integer(bytes, 8);
  if (count > bytes.size() / 4) {
    throw std::runtime_error("invalid stored entry count");
  }
  for (std::uint64_t index = 0; index < count; ++index) {
    auto remaining = bytes;
    const auto length = Integer(remaining, 4);
    if (length > remaining.size() || length > kMaxFrameBytes - 4) {
      throw std::runtime_error("invalid stored frame length");
    }
    const auto message = Decode(bytes.first(static_cast<std::size_t>(length + 4)));
    const auto* append = std::get_if<AppendEntries>(&message.body);
    if (!append || append->entries.size() != 1 || message.term != state.hard_state.term) {
      throw std::runtime_error("invalid stored log entry");
    }
    state.entries.push_back(append->entries.front());
    bytes = bytes.subspan(static_cast<std::size_t>(length + 4));
  }
  if (!bytes.empty()) {
    throw std::runtime_error("trailing bytes in storage image");
  }
  const RaftLog validated(state);
  return state;
}

}  // namespace

FileStorage::FileStorage(std::filesystem::path directory, std::function<void(StoragePoint)> hook)
    : directory_(std::move(directory)), hook_(std::move(hook)) {
  // Require an existing data directory so its creation is outside the publication
  // protocol. The server owns directory provisioning and one storage instance.
  if (!std::filesystem::is_directory(directory_)) {
    throw std::invalid_argument("storage directory must already exist");
  }
  const auto path = directory_ / "storage.bin";
  if (!std::filesystem::exists(path)) {
    return;
  }
  if (std::filesystem::file_size(path) > kMaxImageBytes) {
    throw std::runtime_error("storage image exceeds 64 MiB limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read storage image");
  }
  const Bytes bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) {
    throw std::runtime_error("storage image read failed");
  }
  state_ = Deserialize(bytes);
}

auto FileStorage::Load() -> PersistentState {
  if (stopped_) {
    throw std::runtime_error("storage stopped after failed publication; reopen required");
  }
  return state_;
}

void FileStorage::Persist(const StorageBatch& batch) {
  if (stopped_) {
    throw std::runtime_error("storage stopped after failed publication; reopen required");
  }
  try {
    auto next = ApplyStorageBatch(state_, batch);
    const auto bytes = Serialize(next);
    const auto temporary = directory_ / "storage.tmp";
    platform::WriteSynced(temporary, bytes);
    if (hook_) {
      hook_(StoragePoint::TemporarySynced);
    }
    platform::AtomicReplace(temporary, directory_ / "storage.bin");
    if (hook_) {
      hook_(StoragePoint::Replaced);
    }
    state_ = std::move(next);
  } catch (...) {
    stopped_ = true;
    throw;
  }
}

}  // namespace quorumkv
