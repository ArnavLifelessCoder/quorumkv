#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include "quorumkv/raft_node.hpp"

namespace quorumkv {
namespace {

struct TestClock final : Clock {
  TimePoint time{};
  auto Now() -> TimePoint override { return time; }
};
struct TestRandom final : Random {
  std::uint64_t value{};
  auto Next() -> std::uint64_t override { return value++; }
};
struct TestStorage final : Storage {
  PersistentState state;
  bool fail{};
  std::vector<HardState> writes;
  auto Load() -> PersistentState override { return state; }
  void Persist(const StorageBatch& batch) override {
    if (fail) {
      throw std::runtime_error("injected persistence failure");
    }
    if (batch.hard_state) {
      writes.push_back(*batch.hard_state);
    }
    state = ApplyStorageBatch(state, batch);
  }
};
struct TestTransport final : Transport {
  explicit TestTransport(TestStorage& disk) : storage(disk) {}
  TestStorage& storage;
  std::vector<Message> messages;
  void Send(NodeId to, const Message& message) override {
    EXPECT_EQ(to, message.to);
    EXPECT_EQ(message.term, storage.state.hard_state.term);
    if (std::holds_alternative<RequestVote>(message.body)) {
      EXPECT_EQ(storage.state.hard_state.voted_for, message.from);
    }
    if (const auto* vote = std::get_if<VoteResponse>(&message.body); vote && vote->granted) {
      EXPECT_EQ(storage.state.hard_state.voted_for, message.to);
    }
    messages.push_back(message);
  }
};
struct ElectionTest : testing::Test {
  TestClock clock;
  TestRandom random;
  TestStorage storage;
  TestTransport transport{storage};
  ElectionConfig config{.id = 1, .members = {1, 2, 3}};
  auto MakeNode() -> RaftNode { return {config, transport, clock, storage, random}; }
  static auto From(NodeId from, Term term, MessageBody body) -> Message {
    return {.from = from, .to = 1, .term = term, .rpc_id = 7, .body = std::move(body)};
  }
};

TEST_F(ElectionTest, TimeoutPersistsSelfVoteBeforeSendingAndRetriesInNewTerm) {
  auto node = MakeNode();
  clock.time = 14;
  node.Tick();
  EXPECT_TRUE(transport.messages.empty());
  clock.time = 15;
  node.Tick();
  EXPECT_EQ(node.role(), Role::Candidate);
  EXPECT_EQ(node.hard_state(), (HardState{1, 1}));
  ASSERT_EQ(transport.messages.size(), 2U);
  clock.time = 31;
  node.Tick();
  EXPECT_EQ(node.hard_state(), (HardState{2, 1}));
}

TEST_F(ElectionTest, OnlyDistinctCurrentTermMemberVotesFormMajority) {
  config.members = {1, 2, 3, 4, 5};
  auto node = MakeNode();
  clock.time = 15;
  node.Tick();
  // Member 4 never votes in this term: counting this stale reply adds an extra vote.
  node.Step(From(4, 0, VoteResponse{true}));
  node.Step(From(99, 100, VoteResponse{true}));
  node.Step(From(1, 1, VoteResponse{true}));
  auto wrong_recipient = From(3, 1, VoteResponse{true});
  wrong_recipient.to = 2;
  node.Step(wrong_recipient);
  node.Step(From(2, 1, VoteResponse{true}));
  ASSERT_EQ(node.role(), Role::Candidate);
  node.Step(From(2, 1, VoteResponse{true}));
  node.Step(From(3, 1, VoteResponse{false}));
  EXPECT_EQ(node.role(), Role::Candidate);
  node.Step(From(3, 1, VoteResponse{true}));
  EXPECT_EQ(node.role(), Role::Leader);
  EXPECT_EQ(node.leader_hint(), 1U);
  const auto sent = transport.messages.size();
  clock.time = 18;
  node.Tick();
  EXPECT_EQ(transport.messages.size(), sent + 4);
}

TEST_F(ElectionTest, VoteSurvivesRestartAndHigherTermClearsItBeforeReply) {
  {
    auto node = MakeNode();
    node.Step(From(2, 4, RequestVote{}));
    EXPECT_EQ(node.hard_state(), (HardState{4, 2}));
  }
  auto node = MakeNode();
  node.Step(From(3, 4, RequestVote{}));
  EXPECT_FALSE(std::get<VoteResponse>(transport.messages.back().body).granted);
  node.Step(From(2, 4, RequestVote{}));
  EXPECT_TRUE(std::get<VoteResponse>(transport.messages.back().body).granted);
  node.Step(From(3, 5, RequestVote{}));
  EXPECT_EQ(node.hard_state(), (HardState{5, 3}));
}

TEST_F(ElectionTest, LogFreshnessUsesTermThenIndexIncludingSnapshotTail) {
  storage.state.snapshot =
      Snapshot{.last_included_index = 20, .last_included_term = 3, .state = {}};
  auto node = MakeNode();
  node.Step(From(2, 4, RequestVote{.last_log_index = 100, .last_log_term = 2}));
  EXPECT_FALSE(std::get<VoteResponse>(transport.messages.back().body).granted);
  node.Step(From(2, 4, RequestVote{.last_log_index = 19, .last_log_term = 3}));
  EXPECT_FALSE(std::get<VoteResponse>(transport.messages.back().body).granted);
  node.Step(From(2, 4, RequestVote{.last_log_index = 20, .last_log_term = 3}));
  EXPECT_TRUE(std::get<VoteResponse>(transport.messages.back().body).granted);
  storage.state.entries = {{.index = 21, .term = 5, .command = {}}};
  auto restarted = MakeNode();
  restarted.Step(From(3, 6, RequestVote{.last_log_index = 1000, .last_log_term = 4}));
  EXPECT_FALSE(std::get<VoteResponse>(transport.messages.back().body).granted);
  restarted.Step(From(3, 6, RequestVote{.last_log_index = 1, .last_log_term = 6}));
  EXPECT_TRUE(std::get<VoteResponse>(transport.messages.back().body).granted);
}

TEST_F(ElectionTest, DeniedVoteAndStaleHeartbeatDoNotDelayElection) {
  storage.state.hard_state = {.term = 2, .voted_for = 2};
  auto node = MakeNode();
  clock.time = 14;
  node.Step(From(3, 2, RequestVote{}));
  node.Step(From(2, 1, AppendEntries{}));
  clock.time = 15;
  node.Tick();
  EXPECT_EQ(node.role(), Role::Candidate);
  EXPECT_EQ(node.hard_state().term, 3U);
}

TEST_F(ElectionTest, GrantedVoteAndCurrentHeartbeatResetElectionTimer) {
  auto node = MakeNode();
  clock.time = 14;
  node.Step(From(2, 1, RequestVote{}));
  clock.time = 15;
  node.Tick();
  EXPECT_EQ(node.role(), Role::Follower);
  clock.time = 29;
  node.Step(From(2, 1, AppendEntries{}));
  clock.time = 31;
  node.Tick();
  EXPECT_EQ(node.role(), Role::Follower);
  EXPECT_EQ(node.leader_hint(), 2U);
  clock.time = 46;
  node.Tick();
  EXPECT_EQ(node.role(), Role::Candidate);
}

TEST_F(ElectionTest, SameTermHeartbeatStepsCandidateDownWithoutClearingVote) {
  auto node = MakeNode();
  clock.time = 15;
  node.Tick();
  node.Step(From(2, 1, AppendEntries{}));
  EXPECT_EQ(node.role(), Role::Follower);
  EXPECT_EQ(node.hard_state(), (HardState{1, 1}));
  node.Step(From(3, 1, RequestVote{}));
  EXPECT_FALSE(std::get<VoteResponse>(transport.messages.back().body).granted);
}

TEST_F(ElectionTest, HigherTermResponseStepsLeaderDownButDoesNotResetTimer) {
  auto node = MakeNode();
  clock.time = 15;
  node.Tick();
  node.Step(From(2, 1, VoteResponse{true}));
  ASSERT_EQ(node.role(), Role::Leader);
  clock.time = 31;
  node.Step(From(2, 2, AppendResponse{}));
  EXPECT_EQ(node.role(), Role::Follower);
  EXPECT_EQ(node.hard_state(), (HardState{2, std::nullopt}));
  EXPECT_FALSE(node.leader_hint());
  node.Tick();
  EXPECT_EQ(node.hard_state().term, 3U);
}

TEST_F(ElectionTest, StorageFailureSendsNothingAndLatchesStop) {
  auto node = MakeNode();
  storage.fail = true;
  EXPECT_THROW(node.Step(From(2, 1, RequestVote{})), std::runtime_error);
  EXPECT_TRUE(transport.messages.empty());
  EXPECT_TRUE(node.stopped());
  storage.fail = false;
  EXPECT_THROW(node.Tick(), std::runtime_error);
  EXPECT_THROW(node.Step(From(2, 1, RequestVote{})), std::runtime_error);
}

TEST_F(ElectionTest, FailedSelfVoteCannotSendElectionTraffic) {
  auto node = MakeNode();
  storage.fail = true;
  clock.time = 15;
  EXPECT_THROW(node.Tick(), std::runtime_error);
  EXPECT_TRUE(transport.messages.empty());
  EXPECT_EQ(storage.state.hard_state.term, 0U);
}

TEST_F(ElectionTest, SingleMemberElectsItselfAndTermCannotWrap) {
  config.members = {1};
  auto node = MakeNode();
  clock.time = 15;
  node.Tick();
  EXPECT_EQ(node.role(), Role::Leader);
  EXPECT_TRUE(transport.messages.empty());
  storage.state.hard_state.term = std::numeric_limits<Term>::max();
  auto exhausted = MakeNode();
  clock.time = 100;
  EXPECT_THROW(exhausted.Tick(), std::overflow_error);
  EXPECT_TRUE(exhausted.stopped());
}

TEST_F(ElectionTest, RejectsInvalidMembershipAndTiming) {
  config.members = {2, 3};
  EXPECT_THROW(MakeNode(), std::invalid_argument);
  config.members = {1, 1};
  EXPECT_THROW(MakeNode(), std::invalid_argument);
  config.members = {1};
  config.election_max = 1;
  EXPECT_THROW(MakeNode(), std::invalid_argument);
}

}  // namespace
}  // namespace quorumkv
