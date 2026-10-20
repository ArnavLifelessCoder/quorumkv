#include "quorumkv/replication_safety.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace quorumkv::sim {
namespace {

auto Log(std::initializer_list<LogEntry> entries) -> RaftLog {
  PersistentState state;
  state.entries = entries;
  return RaftLog(state);
}

TEST(ReplicationSafety, MatchingIdentityRequiresExactEntryAndEntirePrefixAcrossHistory) {
  ReplicationSafety safety;
  const auto first =
      Log({{.index = 1, .term = 1, .command = {}}, {.index = 2, .term = 3, .command = {}}});
  safety.ObserveLog(1, first);
  safety.ObserveLog(2, first);
  safety.ObserveLog(1, Log({}));
  EXPECT_THROW(safety.ObserveLog(3, Log({{.index = 1, .term = 2, .command = {}},
                                         {.index = 2, .term = 3, .command = {}}})),
               std::logic_error);
  EXPECT_THROW(safety.ObserveLog(
                   4, Log({{.index = 1, .term = 1, .kind = EntryKind::Command, .command = {7}}})),
               std::logic_error);
}

TEST(ReplicationSafety, CommitsSurviveRestartAndNewLeadersMustContainThem) {
  ReplicationSafety safety;
  const auto first =
      Log({{.index = 1, .term = 1, .command = {}}, {.index = 2, .term = 3, .command = {}}});
  safety.ObserveLog(1, first);
  safety.ObserveCommit(1, first, 2);
  safety.ObserveCommit(1, first, 0);
  safety.ObserveLog(1, first);
  safety.ObserveLeader(2, 4, first);
  EXPECT_THROW(safety.ObserveLog(1, Log({})), std::logic_error);
  EXPECT_THROW(safety.ObserveLeader(3, 5, Log({})), std::logic_error);
  EXPECT_THROW(safety.ObserveCommit(2, first, 3), std::logic_error);
  EXPECT_THROW(safety.ObserveCommit(3, Log({{.index = 1, .term = 2, .command = {}}}), 1),
               std::logic_error);
  EXPECT_EQ(safety.committed().size(), 2U);
}

}  // namespace
}  // namespace quorumkv::sim
