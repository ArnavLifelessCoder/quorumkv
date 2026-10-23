#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <ranges>
#include <stdexcept>

#include "quorumkv/file_storage.hpp"
#include "quorumkv/kv.hpp"
#include "quorumkv/kv_workload.hpp"
#include "quorumkv/sim_cluster.hpp"

namespace quorumkv {
namespace {

auto Command(LogIndex index, Term term, std::uint8_t value) -> LogEntry {
  return {.index = index, .term = term, .kind = EntryKind::Command, .command = {value}};
}

auto Image(std::vector<LogEntry> entries) -> PersistentState {
  PersistentState state;
  state.hard_state = {.term = 3, .voted_for = std::nullopt};
  state.entries = std::move(entries);
  return state;
}

auto SnapshotBatch(LogIndex index, Term term, Bytes state) -> StorageBatch {
  StorageBatch batch;
  batch.snapshot =
      Snapshot{.last_included_index = index, .last_included_term = term, .state = std::move(state)};
  return batch;
}

TEST(SnapshotStorage, MatchingSnapshotKeepsTheSuffixAndOtherwiseReplacesTheLog) {
  const auto log = Image({Command(1, 1, 1), Command(2, 2, 2), Command(3, 3, 3)});
  const auto kept = ApplyStorageBatch(log, SnapshotBatch(2, 2, {9}));
  ASSERT_TRUE(kept.snapshot);
  EXPECT_EQ(kept.entries, (std::vector<LogEntry>{Command(3, 3, 3)}));
  EXPECT_EQ(RaftLog(kept).base_index(), 2U);
  EXPECT_EQ(RaftLog(kept).term(2), 2U);
  // Conflicting term at the snapshot point: the whole log is superseded.
  EXPECT_TRUE(ApplyStorageBatch(log, SnapshotBatch(2, 5, {9})).entries.empty());
  // Beyond the log: the same.
  EXPECT_TRUE(ApplyStorageBatch(log, SnapshotBatch(7, 3, {9})).entries.empty());
  EXPECT_EQ(RaftLog(ApplyStorageBatch(log, SnapshotBatch(7, 3, {}))).last_index(), 7U);
  // A later snapshot at the end of a compacted log keeps nothing; an earlier one is refused.
  const auto compacted = ApplyStorageBatch(kept, SnapshotBatch(3, 3, {8}));
  EXPECT_TRUE(compacted.entries.empty());
  ASSERT_TRUE(compacted.snapshot.has_value());
  const auto latest = compacted.snapshot.value_or(Snapshot{});
  EXPECT_EQ(latest.state, (Bytes{8}));
  EXPECT_THROW(static_cast<void>(ApplyStorageBatch(compacted, SnapshotBatch(2, 2, {}))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(ApplyStorageBatch(log, SnapshotBatch(1, 0, {}))),
               std::invalid_argument);
}

class SnapshotFileTest : public testing::Test {
 protected:
  std::filesystem::path directory;
  void SetUp() override {
    directory = std::filesystem::path(QUORUMKV_TEST_ROOT) / "snapshot-tests" /
                testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
};

TEST_F(SnapshotFileTest, SnapshotAndSuffixSurviveReopen) {
  {
    FileStorage storage(directory);
    StorageBatch batch;
    batch.hard_state = HardState{.term = 3, .voted_for = 1};
    batch.append = {Command(1, 1, 1), Command(2, 3, 2), Command(3, 3, 3)};
    storage.Persist(batch);
    storage.Persist(SnapshotBatch(2, 3, Bytes(70000, 0xab)));
  }
  const auto loaded = FileStorage(directory).Load();
  ASSERT_TRUE(loaded.snapshot.has_value());
  const auto snapshot = loaded.snapshot.value_or(Snapshot{});
  EXPECT_EQ(snapshot.last_included_index, 2U);
  EXPECT_EQ(snapshot.state, Bytes(70000, 0xab));
  EXPECT_EQ(loaded.entries, (std::vector<LogEntry>{Command(3, 3, 3)}));
  EXPECT_EQ(loaded.hard_state, (HardState{.term = 3, .voted_for = 1}));
}

TEST_F(SnapshotFileTest, CorruptSnapshotSectionIsRejected) {
  {
    FileStorage storage(directory);
    StorageBatch batch;
    batch.append = {Command(1, 1, 1)};
    storage.Persist(batch);
    storage.Persist(SnapshotBatch(1, 1, {1, 2, 3}));
  }
  const auto path = directory / "storage.bin";
  std::vector<char> bytes;
  {
    std::ifstream input(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  bytes[bytes.size() - 6] ^= 1;
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  EXPECT_THROW(FileStorage storage(directory), std::runtime_error);
}

struct RecordingTransport final : Transport {
  std::vector<Message> sent;
  void Send(NodeId to, const Message& message) override {
    EXPECT_EQ(to, message.to);
    sent.push_back(message);
  }
};

struct SnapshotNodeTest : testing::Test {
  sim::MemoryStorage storage;
  sim::VirtualClock clock;
  sim::SeededRandom random{0};
  RecordingTransport transport;
  kv::Store machine;
  ElectionConfig config{.id = 1, .members = {1, 2, 3}};
  auto Node() -> RaftNode { return {config, transport, clock, storage, random, &machine}; }
  static auto Install(LogIndex index, std::uint64_t offset, bool done, Bytes data) -> Message {
    return {.from = 2,
            .to = 1,
            .term = 1,
            .rpc_id = 30 + offset,
            .body = InstallSnapshot{.last_included_index = index,
                                    .last_included_term = 1,
                                    .offset = offset,
                                    .done = done,
                                    .data = std::move(data)}};
  }
  auto LastReply() -> SnapshotResponse {
    return std::get<SnapshotResponse>(transport.sent.back().body);
  }
};

auto SourceSnapshot() -> Bytes {
  kv::Store source;
  static_cast<void>(
      source.Apply(kv::EncodeRequest({.client = {.high = 1, .low = 1},
                                      .sequence = 1,
                                      .operation = kv::Put{.key = "k", .value = "v"}})));
  return source.Snapshot();
}

TEST_F(SnapshotNodeTest, FollowerAssemblesChunksInOrderPersistsThenRestores) {
  auto node = Node();
  const auto state = SourceSnapshot();
  const auto half = static_cast<std::ptrdiff_t>(state.size() / 2);
  const Bytes first(state.begin(), state.begin() + half);
  const Bytes second(state.begin() + half, state.end());
  node.Step(Install(5, 0, false, first));
  EXPECT_EQ(LastReply(), (SnapshotResponse{.success = true, .next_offset = first.size()}));
  EXPECT_FALSE(storage.Load().snapshot);
  // A chunk at the wrong offset is refused with the offset the follower expects.
  node.Step(Install(5, first.size() + 1, true, second));
  EXPECT_EQ(LastReply(), (SnapshotResponse{.success = false, .next_offset = first.size()}));
  node.Step(Install(5, first.size(), true, second));
  EXPECT_EQ(LastReply(), (SnapshotResponse{.success = true, .next_offset = state.size()}));
  const auto durable = storage.Load();
  ASSERT_TRUE(durable.snapshot.has_value());
  const auto installed = durable.snapshot.value_or(Snapshot{});
  EXPECT_EQ(installed.state, state);
  EXPECT_EQ(node.commit_index(), 5U);
  EXPECT_EQ(node.last_applied(), 5U);
  EXPECT_EQ(machine.Snapshot(), state);
  // Already covered: acknowledged as complete without touching storage.
  const auto revision = storage.revision();
  node.Step(Install(4, 0, true, {}));
  EXPECT_TRUE(LastReply().success);
  EXPECT_EQ(LastReply().next_offset, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(storage.revision(), revision);
}

TEST_F(SnapshotNodeTest, StaleTermSnapshotIsRejectedWithoutState) {
  storage = sim::MemoryStorage(Image({}));
  auto node = Node();
  node.Step(Install(5, 0, true, SourceSnapshot()));
  EXPECT_FALSE(LastReply().success);
  EXPECT_FALSE(storage.Load().snapshot);
  EXPECT_EQ(transport.sent.back().term, 3U);
}

TEST_F(SnapshotNodeTest, RestartRestoresTheMachineFromTheDurableSnapshot) {
  {
    auto node = Node();
    node.Step(Install(5, 0, true, SourceSnapshot()));
  }
  kv::Store fresh;
  auto restarted = RaftNode(config, transport, clock, storage, random, &fresh);
  EXPECT_EQ(fresh.Snapshot(), SourceSnapshot());
  EXPECT_EQ(restarted.last_applied(), 5U);
  EXPECT_EQ(restarted.log().base_index(), 5U);
}

struct SnapshotSettings {
  LogIndex threshold{};
  std::size_t chunk_bytes{};
};

auto KvCluster(std::uint64_t seed, SnapshotSettings snapshots) -> sim::ClusterConfig {
  return {.seed = seed,
          .size = 3,
          .machine_factory = [] { return std::make_unique<kv::Store>(); },
          .snapshot_threshold = snapshots.threshold,
          .snapshot_chunk_bytes = snapshots.chunk_bytes};
}

auto AwaitLeader(sim::Cluster& cluster) -> NodeId {
  for (unsigned tick = 0; tick < 300; ++tick) {
    cluster.Advance();
    if (const auto leaders = cluster.Leaders(); leaders.size() == 1) {
      return leaders.front();
    }
  }
  throw std::runtime_error("no leader");
}

// One client appending to a single key, one command per tick.
struct Writer {
  std::uint64_t sequence{};
  void operator()(sim::Cluster& cluster, NodeId leader) {
    const auto command = kv::EncodeRequest({.client = {.high = 7, .low = 7},
                                            .sequence = ++sequence,
                                            .operation = kv::Append{.key = "k", .suffix = "x;"}});
    ASSERT_TRUE(cluster.Submit(leader, command));
    cluster.Advance();
  }
};

TEST(SimSnapshot, LaggingFollowerCatchesUpThroughChunkedInstallSnapshot) {
  sim::Cluster cluster(KvCluster(11, {.threshold = 10, .chunk_bytes = 7}));
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  const auto leader = AwaitLeader(cluster);
  const NodeId lagging = leader % 3 + 1;
  cluster.Crash(lagging);
  Writer write;
  for (unsigned command = 0; command < 60; ++command) {
    write(cluster, leader);
  }
  for (unsigned tick = 0; tick < 20; ++tick) {
    cluster.Advance();
  }
  const auto compacted = cluster.Log(leader);
  ASSERT_TRUE(compacted.snapshot.has_value());
  const auto snapshot = compacted.snapshot.value_or(Snapshot{});
  ASSERT_GT(snapshot.last_included_index, 1U);
  cluster.Restart(lagging);
  for (unsigned tick = 0; tick < 200; ++tick) {
    cluster.Advance();
  }
  const auto stats = cluster.Stats();
  EXPECT_GE(stats.snapshot_installs, 1U);
  EXPECT_GT(stats.snapshot_chunks, stats.snapshot_installs);
  ASSERT_TRUE(cluster.Log(lagging).snapshot);
  EXPECT_EQ(cluster.LastApplied(lagging), cluster.LastApplied(leader));
  EXPECT_EQ(cluster.MachineSnapshot(lagging), cluster.MachineSnapshot(leader));
  EXPECT_EQ(stats.steps, stats.safety_checks);
}

TEST(SimSnapshot, RestartFromSnapshotPlusLogReproducesIdenticalState) {
  sim::Cluster cluster(KvCluster(12, {.threshold = 8, .chunk_bytes = 64}));
  cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
  const auto leader = AwaitLeader(cluster);
  Writer write;
  for (unsigned command = 0; command < 37; ++command) {
    write(cluster, leader);
  }
  for (unsigned tick = 0; tick < 20; ++tick) {
    cluster.Advance();
  }
  const auto before = cluster.MachineSnapshot(leader);
  for (NodeId id = 1; id <= 3; ++id) {
    ASSERT_TRUE(cluster.Log(id).snapshot) << id;
    ASSERT_FALSE(cluster.Log(id).entries.empty()) << id;
    cluster.Crash(id);
  }
  const auto restores = cluster.Stats().restore_checks;
  for (NodeId id = 1; id <= 3; ++id) {
    cluster.Restart(id);
  }
  // Every restart compared the restored machine with its durable snapshot.
  EXPECT_EQ(cluster.Stats().restore_checks, restores + 3);
  const auto next = AwaitLeader(cluster);
  for (unsigned tick = 0; tick < 50; ++tick) {
    cluster.Advance();
  }
  for (NodeId id = 1; id <= 3; ++id) {
    EXPECT_EQ(cluster.LastApplied(id), cluster.LastApplied(next)) << id;
    // The new leader's no-op does not change the machine.
    EXPECT_EQ(cluster.MachineSnapshot(id), before) << id;
  }
}

TEST(SimSnapshot, LogStaysBoundedUnderALongRunningWorkload) {
  const auto result = sim::RunKvScenario({.seed = 3,
                                          .snapshot_threshold = 16,
                                          .snapshot_chunk_bytes = 64,
                                          .active_ticks = 1500,
                                          .faults = false});
  std::cout << "[kv-longrun] ticks=1500 threshold=16 operations=" << result.history.size()
            << " applied_commands=" << result.stats.applied_commands
            << " snapshots_taken=" << result.stats.snapshots_taken
            << " max_retained_entries=" << result.stats.max_retained_entries << '\n';
  EXPECT_GT(result.history.size(), 1000U);
  EXPECT_GT(result.stats.snapshots_taken, 50U);
  // Compaction runs every tick, so at most one tick of new entries sits above the threshold.
  EXPECT_LE(result.stats.max_retained_entries, 16U + 16U);
}

}  // namespace
}  // namespace quorumkv
