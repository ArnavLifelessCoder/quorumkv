#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>

#include "quorumkv/types.hpp"

namespace quorumkv::test {

// Deliberately simple valid-input oracle: build each integer in reverse using division,
// construct the payload separately, then prepend its length. No production codec helpers.
inline void ReferenceInteger(Bytes& out, std::uint64_t value, std::size_t width) {
  Bytes digits(width);
  for (auto& digit : digits) {
    digit = static_cast<std::uint8_t>(value % 256);
    value /= 256;
  }
  std::reverse(digits.begin(), digits.end());
  out.insert(out.end(), digits.begin(), digits.end());
}

inline void ReferenceBlob(Bytes& out, const Bytes& bytes) {
  ReferenceInteger(out, bytes.size(), 4);
  out.insert(out.end(), bytes.begin(), bytes.end());
}

inline Bytes ReferenceEncode(const Message& message) {
  Bytes payload{1};
  std::visit(
      [&](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, RequestVote>) {
          payload.push_back(1);
        } else if constexpr (std::is_same_v<T, VoteResponse>) {
          payload.push_back(2);
        } else if constexpr (std::is_same_v<T, AppendEntries>) {
          payload.push_back(3);
        } else if constexpr (std::is_same_v<T, AppendResponse>) {
          payload.push_back(4);
        } else if constexpr (std::is_same_v<T, InstallSnapshot>) {
          payload.push_back(5);
        } else {
          static_assert(std::is_same_v<T, SnapshotResponse>);
          payload.push_back(6);
        }
      },
      message.body);
  for (auto value : {message.from, message.to, message.term, message.rpc_id}) {
    ReferenceInteger(payload, value, 8);
  }
  std::visit(
      [&](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, RequestVote>) {
          ReferenceInteger(payload, body.last_log_index, 8);
          ReferenceInteger(payload, body.last_log_term, 8);
        } else if constexpr (std::is_same_v<T, VoteResponse>) {
          payload.push_back(body.granted ? 1 : 0);
        } else if constexpr (std::is_same_v<T, AppendEntries>) {
          for (auto value :
               {body.prev_log_index, body.prev_log_term, body.leader_commit, body.read_context}) {
            ReferenceInteger(payload, value, 8);
          }
          ReferenceInteger(payload, body.entries.size(), 4);
          for (const auto& entry : body.entries) {
            ReferenceInteger(payload, entry.index, 8);
            ReferenceInteger(payload, entry.term, 8);
            payload.push_back(static_cast<std::uint8_t>(entry.kind));
            ReferenceBlob(payload, entry.command);
          }
        } else if constexpr (std::is_same_v<T, AppendResponse>) {
          payload.push_back(body.success ? 1 : 0);
          ReferenceInteger(payload, body.match_index, 8);
          payload.push_back(body.conflict_term.has_value() ? 1 : 0);
          if (body.conflict_term) {
            ReferenceInteger(payload, *body.conflict_term, 8);
          }
          ReferenceInteger(payload, body.conflict_index, 8);
          ReferenceInteger(payload, body.read_context, 8);
        } else if constexpr (std::is_same_v<T, InstallSnapshot>) {
          for (auto value : {body.last_included_index, body.last_included_term, body.offset}) {
            ReferenceInteger(payload, value, 8);
          }
          payload.push_back(body.done ? 1 : 0);
          ReferenceBlob(payload, body.data);
        } else {
          static_assert(std::is_same_v<T, SnapshotResponse>);
          payload.push_back(body.success ? 1 : 0);
          ReferenceInteger(payload, body.next_offset, 8);
        }
      },
      message.body);
  Bytes frame;
  ReferenceInteger(frame, payload.size(), 4);
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

}  // namespace quorumkv::test
