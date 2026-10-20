#include "quorumkv/log.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace quorumkv {
namespace {

auto Entry(LogIndex index, Term term, std::uint8_t byte = 1) -> LogEntry {
  return {.index = index, .term = term, .kind = EntryKind::Command, .command = {byte}};
}

TEST(RaftLog, TermsAndSlicesRespectTheCompactedBoundary) {
  PersistentState state;
  state.snapshot = Snapshot{.last_included_index = 3, .last_included_term = 2, .state = {}};
  state.entries = {Entry(4, 2), Entry(5, 4), Entry(6, 4)};
  const RaftLog log(state);
  EXPECT_EQ(log.base_index(), 3U);
  EXPECT_EQ(log.last_index(), 6U);
  EXPECT_EQ(log.last_term(), 4U);
  EXPECT_EQ(log.term(3), 2U);
  EXPECT_EQ(log.first_in_term(6), 5U);
  EXPECT_EQ(log.first_in_term(4), 3U);
  EXPECT_EQ(log.last_in_term(2), 4U);
  EXPECT_EQ(log.last_in_term(4), 6U);
  EXPECT_FALSE(log.last_in_term(3));
  EXPECT_EQ(log.slice(5, 100), (std::vector{Entry(5, 4), Entry(6, 4)}));
  EXPECT_TRUE(log.slice(7, 1).empty());
  EXPECT_THROW(static_cast<void>(log.at(3)), std::out_of_range);
  EXPECT_THROW(static_cast<void>(log.term(2)), std::out_of_range);
  EXPECT_THROW(static_cast<void>(log.slice(3, 2)), std::out_of_range);
  EXPECT_THROW(static_cast<void>(log.slice(8, 2)), std::out_of_range);
}

TEST(RaftLog, RejectsGapsTermRegressionAndIndexOverflow) {
  PersistentState state;
  for (const auto& entries : {std::vector{Entry(2, 1)}, std::vector{Entry(1, 2), Entry(2, 1)},
                              std::vector{Entry(1, 0)}}) {
    state.entries = entries;
    EXPECT_THROW(RaftLog{state}, std::invalid_argument);
  }
  state.snapshot = Snapshot{.last_included_index = std::numeric_limits<LogIndex>::max(),
                            .last_included_term = 1,
                            .state = {}};
  state.entries = {Entry(0, 1)};
  EXPECT_THROW(RaftLog{state}, std::invalid_argument);
}

TEST(RaftLog, BatchReplacesOnlyTheRequestedSuffixAndLeavesInputUntouched) {
  PersistentState initial;
  initial.hard_state = {.term = 3, .voted_for = 2};
  initial.entries = {Entry(1, 1), Entry(2, 2), Entry(3, 2)};
  StorageBatch batch;
  batch.hard_state = HardState{.term = 4, .voted_for = 3};
  batch.truncate_from = 2;
  batch.append = {Entry(2, 4, 8), Entry(3, 4, 9)};
  const auto changed = ApplyStorageBatch(initial, batch);
  EXPECT_EQ(changed.entries, (std::vector{Entry(1, 1), Entry(2, 4, 8), Entry(3, 4, 9)}));
  EXPECT_EQ(changed.hard_state, *batch.hard_state);
  EXPECT_EQ(initial.entries.back(), Entry(3, 2));
  batch.append.front().index = 3;
  EXPECT_THROW(static_cast<void>(ApplyStorageBatch(initial, batch)), std::invalid_argument);
  EXPECT_EQ(initial.entries.back(), Entry(3, 2));
}

}  // namespace
}  // namespace quorumkv
