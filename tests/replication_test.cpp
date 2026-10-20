#include <gtest/gtest.h>

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

auto Command(LogIndex index, Term term, std::uint8_t value) -> LogEntry {
  return {.index = index, .term = term, .kind = EntryKind::Command, .command = {value}};
}

struct ReplicationTest : testing::Test {
  sim::MemoryStorage storage;
  sim::VirtualClock clock;
  sim::SeededRandom random{0};
  RecordingTransport transport;
  ElectionConfig config{.id = 1, .members = {1, 2, 3}};
  auto Node() -> RaftNode { return {config, transport, clock, storage, random}; }
  void Elect(RaftNode& node) {
    for (unsigned tick = 0; tick < 31 && node.role() == Role::Follower; ++tick) {
      clock.Advance();
      node.Tick();
    }
    node.Step({.from = 2, .to = 1, .term = node.hard_state().term, .body = VoteResponse{true}});
    ASSERT_EQ(node.role(), Role::Leader);
  }
  static auto Append(std::vector<LogEntry> entries, LogIndex previous = 0, Term previous_term = 0,
                     LogIndex commit = 0, Term term = 1) -> Message {
    return {.from = 2,
            .to = 1,
            .term = term,
            .rpc_id = 10,
            .body = AppendEntries{.prev_log_index = previous,
                                  .prev_log_term = previous_term,
                                  .leader_commit = commit,
                                  .entries = std::move(entries)}};
  }
  auto LatestAppend(NodeId to) -> Message {
    for (auto message = transport.sent.rbegin(); message != transport.sent.rend(); ++message) {
      if (message->to == to && std::holds_alternative<AppendEntries>(message->body)) {
        return *message;
      }
    }
    throw std::logic_error("no append sent");
  }
  static void Ack(RaftNode& node, const Message& request, LogIndex index) {
    node.Step({.from = request.to,
               .to = request.from,
               .term = request.term,
               .rpc_id = request.rpc_id,
               .body = AppendResponse{
                   .success = true, .match_index = index, .conflict_term = std::nullopt}});
  }
};

TEST_F(ReplicationTest, LeaderPersistsNoOpAndCommitsOnlyAcknowledgedEntries) {
  auto node = Node();
  EXPECT_FALSE(node.Propose({7}));
  Elect(node);
  ASSERT_EQ(storage.Load().entries.size(), 1U);
  EXPECT_EQ(storage.Load().entries.front().kind, EntryKind::NoOp);
  EXPECT_EQ(node.commit_index(), 0U);
  const auto request = LatestAppend(2);
  Ack(node, request, 99);
  EXPECT_EQ(node.commit_index(), 0U);
  clock.Advance();
  clock.Advance();
  clock.Advance();
  node.Tick();
  Ack(node, LatestAppend(2), 1);
  EXPECT_EQ(node.commit_index(), 1U);
  EXPECT_EQ(node.Propose({7}), 2U);
  EXPECT_EQ(node.commit_index(), 1U);
  const auto proposal = LatestAppend(2);
  Ack(node, proposal, 2);
  EXPECT_EQ(node.commit_index(), 2U);
  Ack(node, proposal, 2);
  EXPECT_EQ(node.commit_index(), 2U);
}

TEST_F(ReplicationTest, FollowerPersistsBeforeAckAndDoesNotTruncateOnDuplicateOrHeartbeat) {
  auto node = Node();
  const auto request = Append({Command(1, 1, 4), Command(2, 1, 5)});
  node.Step(request);
  EXPECT_EQ(storage.Load().entries.size(), 2U);
  EXPECT_TRUE(std::get<AppendResponse>(transport.sent.back().body).success);
  const auto revision = storage.revision();
  node.Step(request);
  EXPECT_EQ(storage.revision(), revision);
  node.Step(Append({}, 1, 1, 2));
  EXPECT_EQ(node.log().last_index(), 2U);
  EXPECT_EQ(node.commit_index(), 1U);
  EXPECT_EQ(std::get<AppendResponse>(transport.sent.back().body).match_index, 1U);
}

TEST_F(ReplicationTest, ConflictingSuffixIsReplacedAndMissingPrefixReportsHints) {
  auto node = Node();
  node.Step(Append({Command(1, 1, 1), Command(2, 1, 2), Command(3, 1, 3)}));
  node.Step(Append({}, 8, 1, 0, 2));
  auto reply = std::get<AppendResponse>(transport.sent.back().body);
  EXPECT_FALSE(reply.success);
  EXPECT_FALSE(reply.conflict_term);
  EXPECT_EQ(reply.conflict_index, 4U);
  node.Step(Append({}, 3, 2, 0, 2));
  reply = std::get<AppendResponse>(transport.sent.back().body);
  EXPECT_EQ(reply.conflict_term, 1U);
  EXPECT_EQ(reply.conflict_index, 1U);
  node.Step(Append({Command(2, 2, 9)}, 1, 1, 2, 2));
  EXPECT_EQ(node.log().last_index(), 2U);
  EXPECT_EQ(node.log().at(2), Command(2, 2, 9));
  EXPECT_EQ(node.commit_index(), 2U);
  EXPECT_THROW(node.Step(Append({Command(2, 3, 8)}, 1, 1, 2, 3)), std::logic_error);
  EXPECT_EQ(storage.Load().entries.back(), Command(2, 2, 9));
}

TEST_F(ReplicationTest, FailedAppendCannotAcknowledgeOrExposeAnUndurableLog) {
  auto node = Node();
  node.Step(Append({}));
  transport.sent.clear();
  storage.FailNext(sim::SyncFailure::BeforeSync);
  EXPECT_THROW(node.Step(Append({Command(1, 1, 7)})), std::runtime_error);
  EXPECT_TRUE(node.stopped());
  EXPECT_TRUE(transport.sent.empty());
  EXPECT_TRUE(storage.Load().entries.empty());
  storage.Crash();
  auto restarted = Node();
  EXPECT_EQ(restarted.commit_index(), 0U);
  EXPECT_EQ(restarted.log().last_index(), 0U);
}

TEST_F(ReplicationTest, SingleMemberCommitsItsNoOpAndProposalImmediately) {
  config.members = {1};
  auto node = Node();
  for (unsigned tick = 0; tick < 31 && node.role() != Role::Leader; ++tick) {
    clock.Advance();
    node.Tick();
  }
  ASSERT_EQ(node.role(), Role::Leader);
  EXPECT_EQ(node.commit_index(), 1U);
  EXPECT_EQ(node.Propose({8}), 2U);
  EXPECT_EQ(node.commit_index(), 2U);
}

TEST_F(ReplicationTest, MajorityOfOldTermEntriesCannotAdvanceCommit) {
  StorageBatch initial;
  initial.hard_state = HardState{.term = 2, .voted_for = std::nullopt};
  initial.append = {Command(1, 1, 1), Command(2, 2, 2)};
  storage.Persist(initial);
  config.members = {1, 2, 3, 4, 5};
  config.max_append_entries = 1;
  auto node = Node();
  for (unsigned tick = 0; tick < 31 && node.role() == Role::Follower; ++tick) {
    clock.Advance();
    node.Tick();
  }
  for (const auto peer : {NodeId{2}, NodeId{3}}) {
    node.Step({.from = peer, .to = 1, .term = 3, .body = VoteResponse{true}});
  }
  ASSERT_EQ(node.role(), Role::Leader);
  for (const auto peer : {NodeId{2}, NodeId{3}}) {
    auto request = LatestAppend(peer);
    node.Step({.from = peer,
               .to = 1,
               .term = 3,
               .rpc_id = request.rpc_id,
               .body = AppendResponse{.conflict_term = std::nullopt, .conflict_index = 1}});
    Ack(node, LatestAppend(peer), 1);
    Ack(node, LatestAppend(peer), 2);
  }
  EXPECT_EQ(node.commit_index(), 0U);
  Ack(node, LatestAppend(2), 3);
  EXPECT_EQ(node.commit_index(), 0U);
  Ack(node, LatestAppend(3), 3);
  EXPECT_EQ(node.commit_index(), 3U);
}

TEST_F(ReplicationTest, ConflictTermSkipsWholeTermAndStaleRejectionCannotUndoProgress) {
  StorageBatch initial;
  initial.hard_state = HardState{.term = 3, .voted_for = std::nullopt};
  initial.append = {Command(1, 1, 1), Command(2, 2, 2), Command(3, 2, 3), Command(4, 3, 4)};
  storage.Persist(initial);
  auto node = Node();
  Elect(node);
  const auto old = LatestAppend(2);
  clock.Advance();
  clock.Advance();
  clock.Advance();
  node.Tick();
  auto request = LatestAppend(2);
  node.Step({.from = 2,
             .to = 1,
             .term = 4,
             .rpc_id = request.rpc_id,
             .body = AppendResponse{.conflict_term = 2, .conflict_index = 2}});
  request = LatestAppend(2);
  EXPECT_EQ(std::get<AppendEntries>(request.body).prev_log_index, 3U);
  Ack(node, request, 5);
  node.Step({.from = 2,
             .to = 1,
             .term = 4,
             .rpc_id = old.rpc_id,
             .body = AppendResponse{.conflict_term = std::nullopt, .conflict_index = 1}});
  EXPECT_EQ(node.commit_index(), 5U);
  EXPECT_EQ(std::get<AppendEntries>(LatestAppend(2).body).prev_log_index, 5U);
}

TEST_F(ReplicationTest, FailedLeaderAppendNeverSendsAndAfterSyncCrashRetainsEntry) {
  auto node = Node();
  Elect(node);
  transport.sent.clear();
  storage.FailNext(sim::SyncFailure::AfterSync);
  EXPECT_THROW(static_cast<void>(node.Propose({9})), std::runtime_error);
  EXPECT_TRUE(transport.sent.empty());
  EXPECT_TRUE(node.stopped());
  storage.Crash();
  auto restarted = Node();
  EXPECT_EQ(restarted.log().at(2), Command(2, 1, 9));
  EXPECT_EQ(restarted.commit_index(), 0U);
}

TEST_F(ReplicationTest, SendsNextBatchBeforeAcknowledgementAndAcceptsReorderedReplies) {
  config.max_append_entries = 1;
  auto node = Node();
  Elect(node);
  const auto first = LatestAppend(2);
  ASSERT_EQ(node.Propose({2}), 2U);
  const auto second = LatestAppend(2);
  EXPECT_EQ(std::get<AppendEntries>(second.body).prev_log_index, 1U);
  ASSERT_EQ(node.Propose({3}), 3U);
  const auto third = LatestAppend(2);
  EXPECT_EQ(std::get<AppendEntries>(third.body).prev_log_index, 2U);
  EXPECT_EQ(node.commit_index(), 0U);
  Ack(node, third, 3);
  EXPECT_EQ(node.commit_index(), 3U);
  Ack(node, second, 2);
  Ack(node, first, 1);
  EXPECT_EQ(node.commit_index(), 3U);
}

}  // namespace
}  // namespace quorumkv
