#include <gtest/gtest.h>

#include <ranges>
#include <stdexcept>

#include "quorumkv/codec.hpp"
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

struct BatchingTest : testing::Test {
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
  auto Appends(NodeId to) -> std::vector<Message> {
    std::vector<Message> appends;
    for (const auto& message : transport.sent) {
      if (message.to == to && std::holds_alternative<AppendEntries>(message.body)) {
        appends.push_back(message);
      }
    }
    return appends;
  }
  static void Ack(RaftNode& node, const Message& request) {
    const auto& append = std::get<AppendEntries>(request.body);
    const auto last = append.entries.empty() ? append.prev_log_index : append.entries.back().index;
    node.Step({.from = request.to,
               .to = request.from,
               .term = request.term,
               .rpc_id = request.rpc_id,
               .body = AppendResponse{.success = true,
                                      .match_index = last,
                                      .conflict_term = std::nullopt,
                                      .read_context = append.read_context}});
  }
};

TEST_F(BatchingTest, ProposeBatchAppendsEverythingInOneDurableWrite) {
  auto node = Node();
  EXPECT_TRUE(node.ProposeBatch({{1}}).empty());
  Elect(node);
  const auto revision = storage.revision();
  const auto indices = node.ProposeBatch({{1}, {2}, {3}});
  EXPECT_EQ(indices, (std::vector<LogIndex>{2, 3, 4}));
  EXPECT_EQ(storage.revision(), revision + 1);
  ASSERT_EQ(storage.Load().entries.size(), 4U);
  EXPECT_EQ(storage.Load().entries.back().command, (Bytes{3}));
  // The no-op already went out at election; one AppendEntries carries the whole batch.
  const auto latest = Appends(2).back();
  EXPECT_EQ(std::get<AppendEntries>(latest.body).entries.size(), 3U);
  Ack(node, latest);
  EXPECT_EQ(node.commit_index(), 4U);
  EXPECT_THROW(static_cast<void>(node.ProposeBatch({Bytes(kMaxBlobBytes + 1, 0)})),
               std::invalid_argument);
}

TEST_F(BatchingTest, PipelinedLeaderSendsLaterBatchesBeforeAcknowledgements) {
  config.max_append_entries = 1;
  auto node = Node();
  Elect(node);
  const auto before = Appends(2).size();
  ASSERT_TRUE(node.Propose({1}));
  ASSERT_TRUE(node.Propose({2}));
  EXPECT_EQ(Appends(2).size(), before + 2);
}

TEST_F(BatchingTest, WithoutPipeliningOneRequestIsOutstandingUntilAcknowledged) {
  config.max_append_entries = 1;
  config.pipeline = false;
  auto node = Node();
  Elect(node);
  const auto first = Appends(2);
  ASSERT_EQ(first.size(), 1U);  // The no-op.
  ASSERT_TRUE(node.Propose({1}));
  ASSERT_TRUE(node.Propose({2}));
  EXPECT_EQ(Appends(2).size(), 1U);
  Ack(node, first.back());
  ASSERT_EQ(Appends(2).size(), 2U);
  EXPECT_EQ(std::get<AppendEntries>(Appends(2).back().body).entries.front().index, 2U);
  // A lost request is resent from the acknowledged point by the heartbeat timer only.
  for (unsigned tick = 0; tick < 3; ++tick) {
    clock.Advance();
    node.Tick();
  }
  const auto resent = Appends(2).back();
  EXPECT_EQ(std::get<AppendEntries>(resent.body).prev_log_index, 1U);
  Ack(node, resent);
  Ack(node, Appends(2).back());
  EXPECT_EQ(node.commit_index(), 3U);
}

}  // namespace
}  // namespace quorumkv
