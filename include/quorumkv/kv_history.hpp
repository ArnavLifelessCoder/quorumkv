#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace quorumkv::sim {

enum class OpKind : std::uint8_t { Append, Put, Cas, Get };

// One client operation. `invoke`/`complete` are positions in a single global event order,
// so "completed before invoked" is exact even within one simulated tick.
struct HistoryOp {
  std::uint64_t client{};
  OpKind kind{OpKind::Get};
  std::string key;
  // Append: the unique token suffix. Put: the value. Cas: the replacement.
  std::string argument;
  std::string expected;  // Cas only.
  std::uint64_t invoke{};
  std::optional<std::uint64_t> complete;  // Empty: outcome unknown.
  // Get: whether the key existed and its value. Cas: whether it exchanged.
  bool found{};
  std::string value;
  bool exchanged{};
};

// Per-key version checks for histories whose append suffixes are unique tokens ending in ';'.
// An append key's value is a sequence of tokens, so its token count is its version.
// `final_values` are read after quiescence through the same linearizable read path.
// Throws std::logic_error naming the first violated property and the operations involved.
void CheckHistory(const std::vector<HistoryOp>& history,
                  const std::map<std::string, std::string>& final_values);

}  // namespace quorumkv::sim
