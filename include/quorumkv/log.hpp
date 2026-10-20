#pragma once

#include <span>

#include "quorumkv/types.hpp"

namespace quorumkv {

// A contiguous log with an optional compacted prefix described by its snapshot.
class RaftLog {
 public:
  explicit RaftLog(const PersistentState& state = {});
  [[nodiscard]] auto base_index() const -> LogIndex { return base_index_; }
  [[nodiscard]] auto last_index() const -> LogIndex;
  [[nodiscard]] auto last_term() const -> Term;
  [[nodiscard]] auto term(LogIndex index) const -> Term;
  [[nodiscard]] auto at(LogIndex index) const -> const LogEntry&;
  [[nodiscard]] auto entries() const -> std::span<const LogEntry> { return entries_; }
  [[nodiscard]] auto slice(LogIndex first, std::size_t count) const -> std::vector<LogEntry>;
  [[nodiscard]] auto first_in_term(LogIndex index) const -> LogIndex;
  [[nodiscard]] auto last_in_term(Term term) const -> std::optional<LogIndex>;

 private:
  LogIndex base_index_{};
  Term base_term_{};
  std::vector<LogEntry> entries_;
};

// Construct and validate the entire next durable image without changing the input.
// Snapshot publication is introduced with snapshot installation in M5.
[[nodiscard]] auto ApplyStorageBatch(const PersistentState& state,
                                     const StorageBatch& batch) -> PersistentState;

}  // namespace quorumkv
