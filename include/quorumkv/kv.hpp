#pragma once

#include <map>
#include <string>
#include <variant>

#include "quorumkv/interfaces.hpp"

namespace quorumkv::kv {

// Opaque, stable across retries. Never allocated by RaftNode.
struct ClientId {
  std::uint64_t high{};
  std::uint64_t low{};
  auto operator<=>(const ClientId&) const = default;
};

struct Put {
  std::string key;
  std::string value;
  auto operator==(const Put&) const -> bool = default;
};
struct Get {
  std::string key;
  auto operator==(const Get&) const -> bool = default;
};
struct Append {
  std::string key;
  std::string suffix;
  auto operator==(const Append&) const -> bool = default;
};
// Exchanges only when the key exists and holds exactly `expected`.
struct Cas {
  std::string key;
  std::string expected;
  std::string replacement;
  auto operator==(const Cas&) const -> bool = default;
};
using Operation = std::variant<Put, Get, Append, Cas>;

struct Request {
  ClientId client;
  std::uint64_t sequence{};
  Operation operation;
  auto operator==(const Request&) const -> bool = default;
};

struct WriteOk {
  auto operator==(const WriteOk&) const -> bool = default;
};
struct Value {
  bool found{};
  std::string value;
  auto operator==(const Value&) const -> bool = default;
};
// `found`/`previous` describe the key before the operation.
struct CasResult {
  bool exchanged{};
  bool found{};
  std::string previous;
  auto operator==(const CasResult&) const -> bool = default;
};
// A retry older than the client's latest applied sequence. It has no effect.
struct StaleSequence {
  auto operator==(const StaleSequence&) const -> bool = default;
};
// A command that does not decode. It has no effect.
struct Invalid {
  auto operator==(const Invalid&) const -> bool = default;
};
using Response = std::variant<WriteOk, Value, CasResult, StaleSequence, Invalid>;

// Version-tagged, big-endian, length-prefixed encodings. Decoders reject trailing bytes,
// unknown tags, noncanonical booleans, and blobs above kMaxBlobBytes.
auto EncodeRequest(const Request& request) -> Bytes;
auto DecodeRequest(std::span<const std::uint8_t> bytes) -> Request;
auto EncodeRead(const std::string& key) -> Bytes;
auto DecodeRead(std::span<const std::uint8_t> bytes) -> std::string;
auto EncodeResponse(const Response& response) -> Bytes;
auto DecodeResponse(std::span<const std::uint8_t> bytes) -> Response;

// Deterministic replicated state machine. Applying the same committed commands in the
// same order yields the same data, sessions, responses, and digest on every replica.
class Store final : public StateMachine {
 public:
  auto Apply(std::span<const std::uint8_t> command) -> Bytes override;
  [[nodiscard]] auto Read(std::span<const std::uint8_t> query) const -> Bytes override;
  [[nodiscard]] auto Snapshot() const -> Bytes override;
  void Restore(std::span<const std::uint8_t> snapshot) override;

  [[nodiscard]] auto data() const -> const std::map<std::string, std::string>& { return data_; }
  // Rolling hash over every applied command and its response.
  [[nodiscard]] auto digest() const -> std::uint64_t { return digest_; }

 private:
  struct Session {
    std::uint64_t sequence{};
    Bytes response;
    auto operator==(const Session&) const -> bool = default;
  };
  auto Execute(const Operation& operation) -> Response;

  std::map<std::string, std::string> data_;
  std::map<ClientId, Session> sessions_;
  std::uint64_t digest_{14695981039346656037ULL};
};

}  // namespace quorumkv::kv
