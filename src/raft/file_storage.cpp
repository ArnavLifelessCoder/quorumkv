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

// Version 03 records the last journal sequence the image contains. Version 02 added the
// snapshot section; version 01 had neither. Older images remain readable.
constexpr std::array<std::uint8_t, 8> kMagic{'Q', 'K', 'V', 'L', 'O', 'G', '0', '3'};
constexpr std::array<std::uint8_t, 8> kMagicV2{'Q', 'K', 'V', 'L', 'O', 'G', '0', '2'};
constexpr std::array<std::uint8_t, 8> kMagicV1{'Q', 'K', 'V', 'L', 'O', 'G', '0', '1'};
constexpr std::size_t kMaxImageBytes = std::size_t{64} * 1024 * 1024;
constexpr std::size_t kRecordHeader = 4 + 4 + 8;  // length, checksum, sequence

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
    throw std::runtime_error("truncated storage data");
  }
  std::uint64_t value{};
  for (unsigned index = 0; index < width; ++index) {
    value = (value << 8) | bytes[index];
  }
  bytes = bytes.subspan(width);
  return value;
}

void WriteHardState(Bytes& bytes, const HardState& state) {
  Integer<8>(bytes, state.term);
  Integer<1>(bytes, state.voted_for.has_value() ? 1 : 0);
  Integer<8>(bytes, state.voted_for.value_or(0));
}

auto ReadHardState(std::span<const std::uint8_t>& bytes) -> HardState {
  HardState state;
  state.term = Integer(bytes, 8);
  const auto has_vote = Integer(bytes, 1);
  const auto vote = Integer(bytes, 8);
  if (has_vote > 1 || (has_vote == 0 && vote != 0)) {
    throw std::runtime_error("invalid stored vote");
  }
  if (has_vote != 0) {
    state.voted_for = vote;
  }
  return state;
}

void WriteEntries(Bytes& bytes, const std::vector<LogEntry>& entries, std::size_t limit) {
  Integer<8>(bytes, entries.size());
  for (const auto& entry : entries) {
    // Each entry reuses the peer codec, including its validation, as a one-entry frame.
    const auto frame = Encode({.body = AppendEntries{.entries = {entry}}});
    if (frame.size() > limit - bytes.size()) {
      throw std::length_error("storage data exceeds its size limit");
    }
    bytes.insert(bytes.end(), frame.begin(), frame.end());
  }
}

auto ReadEntries(std::span<const std::uint8_t>& bytes) -> std::vector<LogEntry> {
  const auto count = Integer(bytes, 8);
  if (count > bytes.size() / 4) {
    throw std::runtime_error("invalid stored entry count");
  }
  std::vector<LogEntry> entries;
  for (std::uint64_t index = 0; index < count; ++index) {
    auto remaining = bytes;
    const auto length = Integer(remaining, 4);
    if (length > remaining.size() || length > kMaxFrameBytes - 4) {
      throw std::runtime_error("invalid stored frame length");
    }
    const auto message = Decode(bytes.first(static_cast<std::size_t>(length + 4)));
    const auto* append = std::get_if<AppendEntries>(&message.body);
    if (append == nullptr || append->entries.size() != 1) {
      throw std::runtime_error("invalid stored log entry");
    }
    entries.push_back(append->entries.front());
    bytes = bytes.subspan(static_cast<std::size_t>(length) + 4);
  }
  return entries;
}

void WriteSnapshot(Bytes& bytes, const std::optional<Snapshot>& snapshot, std::size_t limit) {
  Integer<1>(bytes, snapshot ? 1 : 0);
  if (snapshot) {
    if (snapshot->state.size() > limit - bytes.size() - 24) {
      throw std::length_error("storage data exceeds its size limit");
    }
    Integer<8>(bytes, snapshot->last_included_index);
    Integer<8>(bytes, snapshot->last_included_term);
    Integer<8>(bytes, snapshot->state.size());
    bytes.insert(bytes.end(), snapshot->state.begin(), snapshot->state.end());
  }
}

auto ReadSnapshot(std::span<const std::uint8_t>& bytes) -> std::optional<Snapshot> {
  const auto has_snapshot = Integer(bytes, 1);
  if (has_snapshot > 1) {
    throw std::runtime_error("invalid stored snapshot flag");
  }
  if (has_snapshot == 0) {
    return std::nullopt;
  }
  Snapshot snapshot;
  snapshot.last_included_index = Integer(bytes, 8);
  snapshot.last_included_term = Integer(bytes, 8);
  const auto size = Integer(bytes, 8);
  if (size > bytes.size() || snapshot.last_included_term == 0) {
    throw std::runtime_error("invalid stored snapshot");
  }
  snapshot.state.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
  bytes = bytes.subspan(static_cast<std::size_t>(size));
  return snapshot;
}

auto SerializeImage(const PersistentState& state, std::uint64_t sequence) -> Bytes {
  Bytes bytes(kMagic.begin(), kMagic.end());
  Integer<8>(bytes, sequence);
  WriteHardState(bytes, state.hard_state);
  WriteEntries(bytes, state.entries, kMaxImageBytes - 4);
  WriteSnapshot(bytes, state.snapshot, kMaxImageBytes - 4);
  Integer<4>(bytes, Checksum(bytes));
  return bytes;
}

struct Image {
  PersistentState state;
  std::uint64_t sequence{};
};

auto DeserializeImage(std::span<const std::uint8_t> bytes) -> Image {
  if (bytes.size() < 37) {
    throw std::runtime_error("invalid storage image header");
  }
  const auto magic = bytes.first(8);
  const bool v1 = std::ranges::equal(magic, kMagicV1);
  const bool v2 = std::ranges::equal(magic, kMagicV2);
  if (!v1 && !v2 && !std::ranges::equal(magic, kMagic)) {
    throw std::runtime_error("invalid storage image header");
  }
  auto checksum_bytes = bytes.last(4);
  if (Integer(checksum_bytes, 4) != Checksum(bytes.first(bytes.size() - 4))) {
    throw std::runtime_error("storage checksum mismatch");
  }
  bytes = bytes.subspan(8, bytes.size() - 12);
  Image image;
  if (!v1 && !v2) {
    image.sequence = Integer(bytes, 8);
  }
  image.state.hard_state = ReadHardState(bytes);
  image.state.entries = ReadEntries(bytes);
  if (!v1) {
    image.state.snapshot = ReadSnapshot(bytes);
  }
  if (!bytes.empty()) {
    throw std::runtime_error("trailing bytes in storage image");
  }
  const RaftLog validated(image.state);
  return image;
}

auto EncodeBatch(const StorageBatch& batch) -> Bytes {
  Bytes bytes;
  Integer<1>(bytes, batch.hard_state ? 1 : 0);
  if (batch.hard_state) {
    WriteHardState(bytes, *batch.hard_state);
  }
  Integer<1>(bytes, batch.truncate_from ? 1 : 0);
  Integer<8>(bytes, batch.truncate_from.value_or(0));
  WriteEntries(bytes, batch.append, kMaxImageBytes);
  WriteSnapshot(bytes, batch.snapshot, kMaxImageBytes);
  return bytes;
}

auto DecodeBatch(std::span<const std::uint8_t> bytes) -> StorageBatch {
  StorageBatch batch;
  const auto has_hard_state = Integer(bytes, 1);
  if (has_hard_state > 1) {
    throw std::runtime_error("invalid journal record");
  }
  if (has_hard_state == 1) {
    batch.hard_state = ReadHardState(bytes);
  }
  const auto has_truncate = Integer(bytes, 1);
  const auto truncate = Integer(bytes, 8);
  if (has_truncate > 1 || (has_truncate == 0 && truncate != 0)) {
    throw std::runtime_error("invalid journal record");
  }
  if (has_truncate == 1) {
    batch.truncate_from = truncate;
  }
  batch.append = ReadEntries(bytes);
  batch.snapshot = ReadSnapshot(bytes);
  if (!bytes.empty()) {
    throw std::runtime_error("trailing bytes in journal record");
  }
  return batch;
}

auto ReadFile(const std::filesystem::path& path) -> Bytes {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read " + path.filename().string());
  }
  Bytes bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) {
    throw std::runtime_error("read failed for " + path.filename().string());
  }
  return bytes;
}

}  // namespace

FileStorage::FileStorage(std::filesystem::path directory, std::function<void(StoragePoint)> hook,
                         std::size_t journal_bytes)
    : directory_(std::move(directory)), hook_(std::move(hook)), journal_limit_(journal_bytes) {
  // Require an existing data directory so its creation is outside the publication
  // protocol. The server owns directory provisioning and one storage instance.
  if (!std::filesystem::is_directory(directory_)) {
    throw std::invalid_argument("storage directory must already exist");
  }
  const auto image_path = directory_ / "storage.bin";
  std::uint64_t image_sequence = 0;
  if (std::filesystem::exists(image_path)) {
    if (std::filesystem::file_size(image_path) > kMaxImageBytes) {
      throw std::runtime_error("storage image exceeds 64 MiB limit");
    }
    const auto bytes = ReadFile(image_path);
    auto image = DeserializeImage(bytes);
    state_ = std::move(image.state);
    image_sequence = image.sequence;
    image_size_ = bytes.size();
  }
  sequence_ = image_sequence;
  const auto journal_path = directory_ / "journal.bin";
  const bool existed = std::filesystem::exists(journal_path);
  std::uint64_t valid = 0;
  std::uint64_t size = 0;
  if (existed) {
    const auto bytes = ReadFile(journal_path);
    size = bytes.size();
    std::span<const std::uint8_t> rest(bytes);
    while (!rest.empty()) {
      auto header = rest;
      if (header.size() < kRecordHeader) {
        break;  // A torn header at the end: the write never completed.
      }
      const auto length = Integer(header, 4);
      const auto checksum = static_cast<std::uint32_t>(Integer(header, 4));
      const auto sequence = Integer(header, 8);
      const bool last = length > header.size() || length == header.size();
      if (length > header.size() ||
          Checksum(rest.subspan(8, 8 + static_cast<std::size_t>(length))) != checksum) {
        if (!last) {
          throw std::runtime_error("storage journal corrupt before its final record");
        }
        break;  // A torn final record was never acknowledged.
      }
      const auto payload = header.first(static_cast<std::size_t>(length));
      if (sequence > sequence_) {
        if (sequence != sequence_ + 1) {
          throw std::runtime_error("storage journal skips a sequence number");
        }
        state_ = ApplyStorageBatch(state_, DecodeBatch(payload));
        sequence_ = sequence;
      }
      rest = header.subspan(static_cast<std::size_t>(length));
      valid = size - rest.size();
    }
  }
  journal_ = std::make_unique<platform::AppendFile>(journal_path);
  if (!existed) {
    platform::SyncDirectory(directory_);
  } else if (valid < size) {
    journal_->Truncate(valid);
  }
  journal_size_ = valid;
}

FileStorage::~FileStorage() = default;

auto FileStorage::Load() -> PersistentState {
  if (stopped_) {
    throw std::runtime_error("storage stopped after failed publication; reopen required");
  }
  return state_;
}

void FileStorage::Compact(const PersistentState& state) {
  const auto bytes = SerializeImage(state, sequence_ + 1);
  const auto temporary = directory_ / "storage.tmp";
  platform::WriteSynced(temporary, bytes);
  if (hook_) {
    hook_(StoragePoint::TemporarySynced);
  }
  platform::AtomicReplace(temporary, directory_ / "storage.bin");
  if (hook_) {
    hook_(StoragePoint::Replaced);
  }
  // Records up to this sequence are now in the image; replay would skip them anyway.
  journal_->Truncate(0);
  journal_size_ = 0;
  image_size_ = bytes.size();
}

void FileStorage::Persist(const StorageBatch& batch) {
  if (stopped_) {
    throw std::runtime_error("storage stopped after failed publication; reopen required");
  }
  try {
    auto next = ApplyStorageBatch(state_, batch);
    // The journal may grow to the larger of its limit and the image before compacting, so
    // rewriting the image costs at most a constant factor over appending. Zero always compacts.
    if (batch.snapshot || journal_limit_ == 0 ||
        journal_size_ >= std::max<std::uint64_t>(journal_limit_, image_size_)) {
      Compact(next);
    } else {
      const auto payload = EncodeBatch(batch);
      Bytes record;
      record.reserve(kRecordHeader + payload.size());
      Integer<4>(record, payload.size());
      Integer<4>(record, 0);  // checksum placeholder
      Integer<8>(record, sequence_ + 1);
      record.insert(record.end(), payload.begin(), payload.end());
      // The checksum covers the sequence number and the payload.
      const auto checksum = Checksum(std::span(record).subspan(8));
      for (unsigned index = 0; index < 4; ++index) {
        record[4 + index] = static_cast<std::uint8_t>(checksum >> (24 - 8 * index));
      }
      journal_->Append(record);
      journal_->Sync();
      if (hook_) {
        hook_(StoragePoint::JournalSynced);
      }
      journal_size_ += record.size();
    }
    state_ = std::move(next);
    ++sequence_;
  } catch (...) {
    stopped_ = true;
    throw;
  }
}

}  // namespace quorumkv
