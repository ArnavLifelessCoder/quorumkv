#include "quorumkv/interfaces.hpp"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

namespace quorumkv {
namespace {

static_assert(std::is_abstract_v<Transport> && std::has_virtual_destructor_v<Transport>);
static_assert(std::is_abstract_v<Clock> && std::has_virtual_destructor_v<Clock>);
static_assert(std::is_abstract_v<Random> && std::has_virtual_destructor_v<Random>);
static_assert(std::is_abstract_v<Storage> && std::has_virtual_destructor_v<Storage>);
static_assert(std::is_abstract_v<StateMachine> && std::has_virtual_destructor_v<StateMachine>);

struct QueueTransport final : Transport {
  std::vector<Message> queue;
  void Send(NodeId to, const Message& message) override {
    EXPECT_EQ(to, message.to);
    queue.push_back(message);
  }
};

TEST(Interfaces, TransportCanOwnMessagesWithoutBorrowingCallerMemory) {
  QueueTransport queue;
  Transport& transport = queue;
  Message message{
      .from = 1,
      .to = 2,
      .term = 3,
      .rpc_id = 4,
      .body = AppendEntries{
          .prev_log_index = 0,
          .prev_log_term = 0,
          .leader_commit = 0,
          .read_context = 7,
          .entries = {{.index = 1, .term = 3, .kind = EntryKind::Command, .command = {0, 255}}}}};
  const auto expected = message;
  transport.Send(message.to, message);
  std::get<AppendEntries>(message.body).entries.clear();
  ASSERT_EQ(queue.queue.size(), 1U);
  EXPECT_EQ(queue.queue.front(), expected);
}

TEST(Types, MissingVoteAndConflictTermDifferFromZero) {
  EXPECT_NE((HardState{3, std::nullopt}), (HardState{3, NodeId{0}}));
  AppendResponse absent;
  AppendResponse zero;
  zero.conflict_term = 0;
  EXPECT_NE(absent, zero);
}

TEST(Types, SnapshotAndLogRetainBinaryDataByValue) {
  PersistentState original{
      .hard_state = {.term = 3, .voted_for = 2},
      .entries = {{.index = 9, .term = 3, .kind = EntryKind::Command, .command = {0, 128, 255}}},
      .snapshot =
          Snapshot{.last_included_index = 8, .last_included_term = 2, .state = {255, 0, 128}}};
  auto copy = original;
  ASSERT_TRUE(copy.snapshot.has_value());
  copy.snapshot->state.clear();
  copy.entries.front().command.clear();
  EXPECT_EQ(original.snapshot->state, (Bytes{255, 0, 128}));
  EXPECT_EQ(original.entries.front().command, (Bytes{0, 128, 255}));
}

}  // namespace
}  // namespace quorumkv
