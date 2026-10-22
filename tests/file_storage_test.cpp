#include "quorumkv/file_storage.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "quorumkv/sim_support.hpp"

namespace quorumkv {
namespace {

class FileStorageTest : public testing::Test {
 protected:
  std::filesystem::path directory;
  void SetUp() override {
    directory = std::filesystem::path(QUORUMKV_TEST_ROOT) / "storage-tests" /
                testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(directory);
    for (const auto* name : {"storage.bin", "storage.tmp", "journal.bin"}) {
      std::filesystem::remove(directory / name);
    }
  }
  void TearDown() override {
    for (const auto* name : {"storage.bin", "storage.tmp", "journal.bin"}) {
      std::filesystem::remove(directory / name);
    }
    std::filesystem::remove(directory);
  }
  void Differential(std::size_t limit);
  static auto Batch() -> StorageBatch {
    StorageBatch batch;
    batch.hard_state = HardState{.term = 2, .voted_for = 3};
    batch.append = {{.index = 1, .term = 1, .command = {}},
                    {.index = 2, .term = 2, .kind = EntryKind::Command, .command = {0, 255, 2}}};
    return batch;
  }
};

TEST_F(FileStorageTest, ReopenRetainsAtomicHardStateAndSuffixReplacement) {
  FileStorage storage(directory);
  EXPECT_EQ(storage.Load(), PersistentState{});
  storage.Persist(Batch());
  EXPECT_EQ(FileStorage(directory).Load(), storage.Load());
  auto replacement = Batch();
  replacement.hard_state = HardState{.term = 3, .voted_for = 1};
  replacement.truncate_from = 2;
  replacement.append = {{.index = 2, .term = 3, .kind = EntryKind::Command, .command = {9}}};
  storage.Persist(replacement);
  const auto reopened = FileStorage(directory).Load();
  EXPECT_EQ(reopened, storage.Load());
  EXPECT_EQ(reopened.entries.size(), 2U);
  EXPECT_EQ(reopened.entries.back(), replacement.append.back());
}

TEST_F(FileStorageTest, InterruptedPublicationRecoversOldOrCompleteNewImage) {
  // A zero journal limit publishes a full image on every batch.
  FileStorage initial(directory, {}, 0);
  initial.Persist(Batch());
  const auto before = initial.Load();
  StorageBatch update;
  update.hard_state = HardState{.term = 4, .voted_for = 1};
  update.append = {{.index = 3, .term = 4, .kind = EntryKind::Command, .command = {4}}};
  for (const auto point : {StoragePoint::TemporarySynced, StoragePoint::Replaced}) {
    FileStorage storage(
        directory,
        [point](StoragePoint current) {
          if (current == point) {
            throw std::runtime_error("injected process loss");
          }
        },
        0);
    EXPECT_THROW(storage.Persist(update), std::runtime_error);
    EXPECT_THROW(static_cast<void>(storage.Load()), std::runtime_error);
    EXPECT_THROW(storage.Persist(update), std::runtime_error);
    const auto recovered = FileStorage(directory).Load();
    if (point == StoragePoint::TemporarySynced) {
      EXPECT_EQ(recovered, before);
    } else {
      EXPECT_EQ(recovered.hard_state, *update.hard_state);
      EXPECT_EQ(recovered.entries.back(), update.append.back());
    }
  }
}

TEST_F(FileStorageTest, RejectsTruncationAndEverySingleByteCorruption) {
  FileStorage storage(directory, {}, 0);
  storage.Persist(Batch());
  const auto path = directory / "storage.bin";
  std::ifstream input(path, std::ios::binary);
  const Bytes original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  input.close();
  const auto write = [&](const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  };
  for (std::size_t index = 0; index < original.size(); ++index) {
    auto corrupt = original;
    corrupt[index] ^= 1U;
    write(corrupt);
    EXPECT_THROW(static_cast<void>(FileStorage(directory)), std::runtime_error);
  }
  for (const auto size : {std::size_t{0}, std::size_t{8}, original.size() - 1}) {
    write(Bytes(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(size)));
    EXPECT_THROW(static_cast<void>(FileStorage(directory)), std::runtime_error);
  }
}

TEST_F(FileStorageTest, DifferentialBatchesMatchMemoryStorageAcrossReopens) {
  for (const std::size_t limit : {std::size_t{0}, std::size_t{300}, kDefaultJournalBytes}) {
    SCOPED_TRACE(limit);
    std::filesystem::remove(directory / "storage.bin");
    std::filesystem::remove(directory / "journal.bin");
    Differential(limit);
  }
}

void FileStorageTest::Differential(std::size_t limit) {
  sim::MemoryStorage reference;
  sim::SeededRandom random(303);
  for (Term term = 1; term <= 50; ++term) {
    const auto previous = reference.Load();
    const auto keep = previous.entries.empty() ? 0 : random.Next() % (previous.entries.size() + 1);
    StorageBatch batch;
    batch.hard_state = HardState{.term = term, .voted_for = 1 + random.Next() % 5};
    batch.truncate_from = keep + 1;
    for (LogIndex index = keep + 1; index <= keep + 1 + term % 3; ++index) {
      batch.append.push_back({.index = index,
                              .term = term,
                              .kind = EntryKind::Command,
                              .command = {static_cast<std::uint8_t>(random.Next())}});
    }
    reference.Persist(batch);
    FileStorage storage(directory, {}, limit);
    storage.Persist(batch);
    EXPECT_EQ(storage.Load(), reference.Load());
    EXPECT_EQ(FileStorage(directory, {}, limit).Load(), reference.Load());
  }
}

auto Entry(LogIndex index, std::uint8_t value) -> LogEntry {
  return {.index = index, .term = 2, .kind = EntryKind::Command, .command = {value}};
}

auto Append(LogIndex index, std::uint8_t value) -> StorageBatch {
  StorageBatch batch;
  batch.append = {Entry(index, value)};
  return batch;
}

auto JournalBytes(const std::filesystem::path& directory) -> Bytes {
  std::ifstream input(directory / "journal.bin", std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteJournal(const std::filesystem::path& directory, const Bytes& bytes) {
  std::ofstream output(directory / "journal.bin", std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

TEST_F(FileStorageTest, JournalRecordsAppendWithoutRewritingTheImage) {
  {
    FileStorage storage(directory);
    storage.Persist(Batch());
    storage.Persist(Append(3, 3));
  }
  EXPECT_FALSE(std::filesystem::exists(directory / "storage.bin"));
  const auto loaded = FileStorage(directory).Load();
  ASSERT_EQ(loaded.entries.size(), 3U);
  EXPECT_EQ(loaded.entries.back(), Entry(3, 3));
  EXPECT_EQ(loaded.hard_state, (HardState{.term = 2, .voted_for = 3}));
}

TEST_F(FileStorageTest, LossRightAfterJournalSyncKeepsTheBatch) {
  {
    FileStorage storage(directory, [](StoragePoint point) {
      if (point == StoragePoint::JournalSynced) {
        throw std::runtime_error("injected process loss");
      }
    });
    EXPECT_THROW(storage.Persist(Batch()), std::runtime_error);
    EXPECT_THROW(static_cast<void>(storage.Load()), std::runtime_error);
  }
  EXPECT_EQ(FileStorage(directory).Load().entries.size(), 2U);
}

TEST_F(FileStorageTest, TornFinalRecordIsDroppedAndEarlierDamageIsRejected) {
  {
    FileStorage storage(directory);
    storage.Persist(Batch());
    storage.Persist(Append(3, 3));
  }
  const auto full = JournalBytes(directory);
  // Every prefix that cuts the final record recovers exactly the first batch.
  for (const auto cut : {full.size() - 1, full.size() - 5, full.size() - 20}) {
    WriteJournal(directory, Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)));
    const auto recovered = FileStorage(directory).Load();
    EXPECT_EQ(recovered.entries.size(), 2U) << cut;
    // The torn tail was cut off, so appending afterwards produces a well-formed journal.
    FileStorage storage(directory);
    storage.Persist(Append(3, 7));
    EXPECT_EQ(FileStorage(directory).Load().entries.back(), Entry(3, 7));
  }
  auto damaged = full;
  damaged[20] ^= 1U;  // Inside the first of two records.
  WriteJournal(directory, damaged);
  EXPECT_THROW(FileStorage storage(directory), std::runtime_error);
  auto last = full;
  last.back() ^= 1U;  // The final record cannot be told apart from a torn write.
  WriteJournal(directory, last);
  EXPECT_EQ(FileStorage(directory).Load().entries.size(), 2U);
}

TEST_F(FileStorageTest, CompactionCrashBeforeJournalTruncationNeverReappliesABatch) {
  {
    FileStorage storage(directory);
    storage.Persist(Batch());
    storage.Persist(Append(3, 3));
  }
  {
    // A one-byte limit compacts on the next batch; stop right after the image is published.
    FileStorage storage(
        directory,
        [](StoragePoint point) {
          if (point == StoragePoint::Replaced) {
            throw std::runtime_error("injected process loss");
          }
        },
        1);
    EXPECT_THROW(storage.Persist(Append(4, 4)), std::runtime_error);
  }
  EXPECT_FALSE(JournalBytes(directory).empty());
  const auto recovered = FileStorage(directory).Load();
  ASSERT_EQ(recovered.entries.size(), 4U);
  EXPECT_EQ(recovered.entries.back(), Entry(4, 4));
}

}  // namespace
}  // namespace quorumkv
