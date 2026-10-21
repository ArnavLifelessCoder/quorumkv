#include "quorumkv/kv_history.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace quorumkv::sim {
namespace {

// Streams every part, so messages need no chains of temporary strings.
template <class... Parts>
[[noreturn]] void Violation(std::string_view property, const Parts&... parts) {
  std::ostringstream out;
  out << property << ": ";
  (out << ... << parts);
  throw std::logic_error(out.str());
}

auto Describe(const HistoryOp& op) -> std::string {
  static constexpr std::array<std::string_view, 4> kNames{"append", "put", "cas", "get"};
  std::ostringstream out;
  out << "client " << op.client << ' ' << kNames.at(static_cast<std::size_t>(op.kind)) << '('
      << op.key;
  if (op.kind != OpKind::Get) {
    out << ", " << op.argument;
  }
  out << ") invoke=" << op.invoke << " complete=";
  if (op.complete) {
    out << *op.complete;
  } else {
    out << "unknown";
  }
  if (op.kind == OpKind::Get) {
    out << " returned ";
    if (op.found) {
      out << '"' << op.value << '"';
    } else {
      out << "missing";
    }
  }
  return out.str();
}

auto Tokens(const std::string& value, const std::string& key) -> std::vector<std::string> {
  std::vector<std::string> tokens;
  std::size_t start = 0;
  while (start < value.size()) {
    const auto end = value.find(';', start);
    if (end == std::string::npos) {
      Violation("Malformed append value", "key ", key, " value \"", value, '"');
    }
    tokens.push_back(value.substr(start, end - start + 1));
    start = end + 1;
  }
  return tokens;
}

void CheckAppendKey(const std::string& key, const std::vector<const HistoryOp*>& ops,
                    const std::string& final_value) {
  std::map<std::string, const HistoryOp*> appends;
  for (const auto* op : ops) {
    if (op->kind == OpKind::Append && !appends.emplace(op->argument, op).second) {
      Violation("Workload error", "append token reused: ", op->argument);
    }
  }
  const auto final_tokens = Tokens(final_value, key);
  std::map<std::string, std::size_t> position;
  for (std::size_t index = 0; index < final_tokens.size(); ++index) {
    const auto& token = final_tokens[index];
    if (!appends.contains(token)) {
      Violation("Fabricated write", "key ", key, " contains token ", token,
                " that no client appended");
    }
    if (!position.emplace(token, index).second) {
      Violation("Exactly-once", "key ", key, " applied ", Describe(*appends.at(token)),
                " more than once");
    }
  }
  for (const auto& [token, op] : appends) {
    if (op->complete && !position.contains(token)) {
      Violation("Durability", "acknowledged ", Describe(*op), " is missing from key ", key);
    }
  }
  // Real-time order: an append acknowledged before another was invoked must precede it.
  auto later_minimum = std::numeric_limits<std::uint64_t>::max();
  const HistoryOp* later_op = nullptr;
  for (auto index = final_tokens.size(); index > 0; --index) {
    const auto* op = appends.at(final_tokens[index - 1]);
    if (later_op != nullptr && later_minimum < op->invoke) {
      Violation("Real-time order", Describe(*later_op), " completed before ", Describe(*op),
                " was invoked, but was applied after it");
    }
    if (op->complete && *op->complete < later_minimum) {
      later_minimum = *op->complete;
      later_op = op;
    }
  }
  for (const auto* read : ops) {
    if (read->kind != OpKind::Get || !read->complete) {
      continue;
    }
    const auto read_tokens = read->found ? Tokens(read->value, key) : std::vector<std::string>{};
    if (read_tokens.size() > final_tokens.size() ||
        !std::equal(read_tokens.begin(), read_tokens.end(), final_tokens.begin())) {
      Violation("Read of a state that never committed", Describe(*read),
                " is not a prefix of the final value \"", final_value, '"');
    }
    const std::set<std::string> seen(read_tokens.begin(), read_tokens.end());
    for (const auto& [token, op] : appends) {
      if (op->complete && *op->complete < read->invoke && !seen.contains(token)) {
        Violation("Stale read", Describe(*read), " misses ", Describe(*op),
                  ", which completed before the read was invoked");
      }
      if (seen.contains(token) && op->invoke > *read->complete) {
        Violation("Read from the future", Describe(*read), " observed ", Describe(*op));
      }
    }
  }
}

void CheckRegisterKey(const std::vector<const HistoryOp*>& ops) {
  for (const auto* read : ops) {
    if (read->kind != OpKind::Get || !read->complete) {
      continue;
    }
    bool explained = !read->found;
    for (const auto* write : ops) {
      const bool writes = write->kind == OpKind::Put ||
                          (write->kind == OpKind::Cas && (write->exchanged || !write->complete));
      if (!writes) {
        continue;
      }
      if (read->found && write->argument == read->value && write->invoke < *read->complete) {
        explained = true;
      }
      // Keys are never deleted, so a missing read after an acknowledged write is stale.
      if (!read->found && write->complete && *write->complete < read->invoke) {
        Violation("Stale read", Describe(*read), " after acknowledged ", Describe(*write));
      }
    }
    if (!explained) {
      Violation("Unexplained read", Describe(*read), " returned a value no write produced");
    }
  }
}

}  // namespace

void CheckHistory(const std::vector<HistoryOp>& history,
                  const std::map<std::string, std::string>& final_values) {
  std::map<std::string, std::vector<const HistoryOp*>> by_key;
  for (const auto& op : history) {
    if (op.complete && *op.complete < op.invoke) {
      Violation("Workload error", Describe(op), " completed before it was invoked");
    }
    by_key[op.key].push_back(&op);
  }
  for (const auto& [key, ops] : by_key) {
    const bool append =
        std::ranges::any_of(ops, [](const auto* op) { return op->kind == OpKind::Append; });
    const bool register_write = std::ranges::any_of(
        ops, [](const auto* op) { return op->kind == OpKind::Put || op->kind == OpKind::Cas; });
    if (append && register_write) {
      Violation("Workload error", "key ", key, " mixes append and register operations");
    }
    if (append) {
      const auto found = final_values.find(key);
      CheckAppendKey(key, ops, found == final_values.end() ? std::string{} : found->second);
    } else {
      CheckRegisterKey(ops);
    }
  }
}

}  // namespace quorumkv::sim
