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
  Message message{1, 2, 3, 4, AppendEntries{0, 0, 0, 7, {{1, 3, EntryKind::Command, {0, 255}}}}};
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
      {3, 2}, {{9, 3, EntryKind::Command, {0, 128, 255}}}, Snapshot{8, 2, {255, 0, 128}}};
  auto copy = original;
  ASSERT_TRUE(copy.snapshot.has_value());
  copy.snapshot->state.clear();
  copy.entries.front().command.clear();
  EXPECT_EQ(original.snapshot->state, (Bytes{255, 0, 128}));
  EXPECT_EQ(original.entries.front().command, (Bytes{0, 128, 255}));
}

}  // namespace
}  // namespace quorumkv
