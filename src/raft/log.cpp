#include "quorumkv/log.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace quorumkv {

RaftLog::RaftLog(const PersistentState& state)
    : base_index_(state.snapshot ? state.snapshot->last_included_index : 0),
      base_term_(state.snapshot ? state.snapshot->last_included_term : 0),
      entries_(state.entries) {
  auto index = base_index_;
  auto previous_term = base_term_;
  for (const auto& entry : entries_) {
    if (index == std::numeric_limits<LogIndex>::max() || entry.index != index + 1 ||
        entry.term == 0 || entry.term < previous_term ||
        (entry.kind != EntryKind::NoOp && entry.kind != EntryKind::Command) ||
        (entry.kind == EntryKind::NoOp && !entry.command.empty())) {
      throw std::invalid_argument("invalid contiguous Raft log");
    }
    index = entry.index;
    previous_term = entry.term;
  }
}

auto RaftLog::last_index() const -> LogIndex {
  return entries_.empty() ? base_index_ : entries_.back().index;
}
auto RaftLog::last_term() const -> Term {
  return entries_.empty() ? base_term_ : entries_.back().term;
}
auto RaftLog::term(LogIndex index) const -> Term {
  return index == base_index_ ? base_term_ : at(index).term;
}
auto RaftLog::at(LogIndex index) const -> const LogEntry& {
  if (index <= base_index_ || index > last_index()) {
    throw std::out_of_range("log index outside retained entries");
  }
  return entries_.at(static_cast<std::size_t>(index - base_index_ - 1));
}
auto RaftLog::slice(LogIndex first, std::size_t count) const -> std::vector<LogEntry> {
  if (first <= base_index_ || first - 1 > last_index()) {
    throw std::out_of_range("log slice outside retained entries");
  }
  const auto offset = static_cast<std::size_t>(first - base_index_ - 1);
  const auto length = std::min(count, entries_.size() - offset);
  const auto begin = entries_.begin() + static_cast<std::ptrdiff_t>(offset);
  return {begin, begin + static_cast<std::ptrdiff_t>(length)};
}
auto RaftLog::first_in_term(LogIndex index) const -> LogIndex {
  const auto wanted = term(index);
  while (index > base_index_ && term(index - 1) == wanted) {
    --index;
  }
  return index;
}
auto RaftLog::last_in_term(Term wanted) const -> std::optional<LogIndex> {
  for (const auto& entry : std::views::reverse(entries_)) {
    if (entry.term == wanted) {
      return entry.index;
    }
  }
  return base_term_ == wanted ? std::optional<LogIndex>{base_index_} : std::nullopt;
}

auto ApplyStorageBatch(const PersistentState& state, const StorageBatch& batch) -> PersistentState {
  const RaftLog before(state);
  if (batch.snapshot) {
    throw std::invalid_argument("snapshot publication requires M5 storage support");
  }
  auto result = state;
  if (batch.truncate_from) {
    const auto first = *batch.truncate_from;
    if (first <= before.base_index() || first - 1 > before.last_index()) {
      throw std::invalid_argument("invalid log truncation boundary");
    }
    result.entries.resize(static_cast<std::size_t>(first - before.base_index() - 1));
  }
  result.entries.insert(result.entries.end(), batch.append.begin(), batch.append.end());
  if (batch.hard_state) {
    result.hard_state = *batch.hard_state;
  }
  const RaftLog validated(result);
  return result;
}

}  // namespace quorumkv
