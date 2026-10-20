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
    std::filesystem::remove(directory / "storage.bin");
    std::filesystem::remove(directory / "storage.tmp");
  }
  void TearDown() override {
    std::filesystem::remove(directory / "storage.bin");
    std::filesystem::remove(directory / "storage.tmp");
    std::filesystem::remove(directory);
  }
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
  FileStorage initial(directory);
  initial.Persist(Batch());
  const auto before = initial.Load();
  StorageBatch update;
  update.hard_state = HardState{.term = 4, .voted_for = 1};
  update.append = {{.index = 3, .term = 4, .kind = EntryKind::Command, .command = {4}}};
  for (const auto point : {StoragePoint::TemporarySynced, StoragePoint::Replaced}) {
    FileStorage storage(directory, [point](StoragePoint current) {
      if (current == point) {
        throw std::runtime_error("injected process loss");
      }
    });
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
  FileStorage storage(directory);
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
    FileStorage storage(directory);
    storage.Persist(batch);
    EXPECT_EQ(storage.Load(), reference.Load());
    EXPECT_EQ(FileStorage(directory).Load(), reference.Load());
  }
}

}  // namespace
}  // namespace quorumkv
