#include <gtest/gtest.h>

#include <ranges>
#include <stdexcept>

#include "quorumkv/sim_support.hpp"

namespace quorumkv {
namespace {

struct RecordingTransport final : Transport {
  std::vector<Message> sent;
  void Send(NodeId to, const Message& message) override {
    EXPECT_EQ(to, message.to);
    sent.push_back(message);
  }
};

// Responds to each command with its first byte plus 100, and to reads with the apply count.
struct RecordingMachine final : StateMachine {
  std::vector<Bytes> commands;
  auto Apply(std::span<const std::uint8_t> command) -> Bytes override {
    commands.emplace_back(command.begin(), command.end());
    return {static_cast<std::uint8_t>(command.front() + 100)};
  }
  [[nodiscard]] auto Read(std::span<const std::uint8_t> /*query*/) const -> Bytes override {
    return {static_cast<std::uint8_t>(commands.size())};
  }
  [[nodiscard]] auto Snapshot() const -> Bytes override { return {}; }
  void Restore(std::span<const std::uint8_t> /*snapshot*/) override {}
};

auto Command(LogIndex index, Term term, std::uint8_t value) -> LogEntry {
  return {.index = index, .term = term, .kind = EntryKind::Command, .command = {value}};
}

struct ReadIndexTest : testing::Test {
  sim::MemoryStorage storage;
  sim::VirtualClock clock;
  sim::SeededRandom random{0};
  RecordingTransport transport;
  RecordingMachine machine;
  ElectionConfig config{.id = 1, .members = {1, 2, 3}};
  auto Node() -> RaftNode { return {config, transport, clock, storage, random, &machine}; }
  void Elect(RaftNode& node) {
    for (unsigned tick = 0; tick < 31 && node.role() == Role::Follower; ++tick) {
      clock.Advance();
      node.Tick();
    }
    node.Step({.from = 2, .to = 1, .term = node.hard_state().term, .body = VoteResponse{true}});
    ASSERT_EQ(node.role(), Role::Leader);
  }
  auto LatestAppend(NodeId to) -> Message {
    for (const auto& message : std::views::reverse(transport.sent)) {
      if (message.to == to && std::holds_alternative<AppendEntries>(message.body)) {
        return message;
      }
    }
    throw std::logic_error("no append sent");
  }
  // Replies exactly as a follower would: echoing the RPC id and the request's read context.
  static void Ack(RaftNode& node, const Message& request, LogIndex index) {
    node.Step({.from = request.to,
               .to = request.from,
               .term = request.term,
               .rpc_id = request.rpc_id,
               .body = AppendResponse{
                   .success = true,
                   .match_index = index,
                   .conflict_term = std::nullopt,
                   .read_context = std::get<AppendEntries>(request.body).read_context}});
  }
  static auto Append(std::vector<LogEntry> entries, LogIndex previous, Term previous_term,
                     LogIndex commit) -> Message {
    return {.from = 2,
            .to = 1,
            .term = 1,
            .rpc_id = 10,
            .body = AppendEntries{.prev_log_index = previous,
                                  .prev_log_term = previous_term,
                                  .leader_commit = commit,
                                  .entries = std::move(entries)}};
  }
};

TEST_F(ReadIndexTest, FollowerAppliesOnlyCommittedEntriesOnceInLogOrder) {
  auto node = Node();
  node.Step(Append({Command(1, 1, 1), Command(2, 1, 2), Command(3, 1, 3)}, 0, 0, 0));
  EXPECT_TRUE(node.TakeApplied().empty());
  EXPECT_TRUE(machine.commands.empty());
  node.Step(Append({}, 3, 1, 2));
  EXPECT_EQ(node.TakeApplied(), (std::vector<Applied>{{.index = 1, .term = 1, .response = {101}},
                                                      {.index = 2, .term = 1, .response = {102}}}));
  node.Step(Append({}, 3, 1, 2));
  EXPECT_TRUE(node.TakeApplied().empty());
  node.Step(Append({}, 3, 1, 3));
  EXPECT_EQ(node.TakeApplied(), (std::vector<Applied>{{.index = 3, .term = 1, .response = {103}}}));
  EXPECT_EQ(machine.commands, (std::vector<Bytes>{{1}, {2}, {3}}));
  EXPECT_EQ(node.last_applied(), 3U);
}

TEST_F(ReadIndexTest, NonLeadersRejectReadsUnlessUnsafeLocalReadsAreEnabled) {
  auto follower = Node();
  EXPECT_FALSE(follower.Read(1, {}));
  config.unsafe_local_reads = true;
  auto unsafe = Node();
  EXPECT_TRUE(unsafe.Read(2, {}));
  EXPECT_EQ(unsafe.TakeReads(), (std::vector<ReadResult>{{.id = 2, .ok = true, .response = {0}}}));
}

TEST_F(ReadIndexTest, NewLeaderWaitsForItsNoOpAndAFreshMajorityHeartbeat) {
  auto node = Node();
  Elect(node);
  const auto before = LatestAppend(2);
  ASSERT_TRUE(node.Read(7, {}));
  EXPECT_TRUE(node.TakeReads().empty());
  // A reply to a heartbeat sent before the read commits the no-op but cannot confirm it.
  Ack(node, before, 1);
  EXPECT_EQ(node.commit_index(), 1U);
  EXPECT_TRUE(node.TakeReads().empty());
  const auto after = LatestAppend(2);
  ASSERT_EQ(std::get<AppendEntries>(after.body).read_context, 1U);
  Ack(node, after, 1);
  EXPECT_EQ(node.TakeReads(), (std::vector<ReadResult>{{.id = 7, .ok = true, .response = {0}}}));
}

TEST_F(ReadIndexTest, ReadWaitsUntilItsReadIndexIsApplied) {
  auto node = Node();
  Elect(node);
  Ack(node, LatestAppend(2), 1);
  ASSERT_EQ(node.Propose({5}), 2U);
  Ack(node, LatestAppend(2), 2);
  ASSERT_EQ(node.TakeApplied().size(), 1U);
  ASSERT_TRUE(node.Read(8, {}));
  Ack(node, LatestAppend(3), 2);
  EXPECT_EQ(node.TakeReads(), (std::vector<ReadResult>{{.id = 8, .ok = true, .response = {1}}}));
}

TEST_F(ReadIndexTest, SteppingDownFailsPendingReads) {
  auto node = Node();
  Elect(node);
  ASSERT_TRUE(node.Read(9, {}));
  node.Step({.from = 3, .to = 1, .term = 5, .body = AppendResponse{}});
  EXPECT_EQ(node.role(), Role::Follower);
  EXPECT_EQ(node.TakeReads(), (std::vector<ReadResult>{{.id = 9, .ok = false, .response = {}}}));
  EXPECT_FALSE(node.Read(10, {}));
}

TEST_F(ReadIndexTest, SingleMemberServesReadsAfterCommittingItsNoOp) {
  config.members = {1};
  auto node = Node();
  for (unsigned tick = 0; tick < 31 && node.role() != Role::Leader; ++tick) {
    clock.Advance();
    node.Tick();
  }
  ASSERT_EQ(node.role(), Role::Leader);
  ASSERT_TRUE(node.Read(3, {}));
  EXPECT_EQ(node.TakeReads(), (std::vector<ReadResult>{{.id = 3, .ok = true, .response = {0}}}));
}

TEST_F(ReadIndexTest, RestartReappliesCommittedPrefixIntoAFreshMachine) {
  {
    auto node = Node();
    node.Step(Append({Command(1, 1, 1), Command(2, 1, 2)}, 0, 0, 2));
    ASSERT_EQ(node.TakeApplied().size(), 2U);
  }
  machine.commands.clear();
  auto restarted = Node();
  EXPECT_EQ(restarted.last_applied(), 0U);
  restarted.Step(Append({}, 2, 1, 2));
  EXPECT_EQ(restarted.TakeApplied().size(), 2U);
  EXPECT_EQ(machine.commands, (std::vector<Bytes>{{1}, {2}}));
}

}  // namespace
}  // namespace quorumkv
