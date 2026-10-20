#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>

#include "quorumkv/types.hpp"

namespace quorumkv::test {

// Deliberately simple valid-input oracle: build each integer in reverse using division,
// construct the payload separately, then prepend its length. No production codec helpers.
template <std::size_t Width>
inline void ReferenceInteger(Bytes& out, std::uint64_t value) {
  Bytes digits(Width);
  for (auto& digit : digits) {
    digit = static_cast<std::uint8_t>(value % 256);
    value /= 256;
  }
  std::ranges::reverse(digits);
  out.insert(out.end(), digits.begin(), digits.end());
}

inline void ReferenceBlob(Bytes& out, const Bytes& bytes) {
  ReferenceInteger<4>(out, bytes.size());
  out.insert(out.end(), bytes.begin(), bytes.end());
}

inline auto ReferenceEncode(const Message& message) -> Bytes {
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
    ReferenceInteger<8>(payload, value);
  }
  std::visit(
      [&](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, RequestVote>) {
          ReferenceInteger<8>(payload, body.last_log_index);
          ReferenceInteger<8>(payload, body.last_log_term);
        } else if constexpr (std::is_same_v<T, VoteResponse>) {
          payload.push_back(body.granted ? 1 : 0);
        } else if constexpr (std::is_same_v<T, AppendEntries>) {
          for (auto value :
               {body.prev_log_index, body.prev_log_term, body.leader_commit, body.read_context}) {
            ReferenceInteger<8>(payload, value);
          }
          ReferenceInteger<4>(payload, body.entries.size());
          for (const auto& entry : body.entries) {
            ReferenceInteger<8>(payload, entry.index);
            ReferenceInteger<8>(payload, entry.term);
            payload.push_back(static_cast<std::uint8_t>(entry.kind));
            ReferenceBlob(payload, entry.command);
          }
        } else if constexpr (std::is_same_v<T, AppendResponse>) {
          payload.push_back(body.success ? 1 : 0);
          ReferenceInteger<8>(payload, body.match_index);
          payload.push_back(body.conflict_term.has_value() ? 1 : 0);
          if (body.conflict_term) {
            ReferenceInteger<8>(payload, *body.conflict_term);
          }
          ReferenceInteger<8>(payload, body.conflict_index);
          ReferenceInteger<8>(payload, body.read_context);
        } else if constexpr (std::is_same_v<T, InstallSnapshot>) {
          for (auto value : {body.last_included_index, body.last_included_term, body.offset}) {
            ReferenceInteger<8>(payload, value);
          }
          payload.push_back(body.done ? 1 : 0);
          ReferenceBlob(payload, body.data);
        } else {
          static_assert(std::is_same_v<T, SnapshotResponse>);
          payload.push_back(body.success ? 1 : 0);
          ReferenceInteger<8>(payload, body.next_offset);
        }
      },
      message.body);
  Bytes frame;
  ReferenceInteger<4>(frame, payload.size());
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

}  // namespace quorumkv::test
