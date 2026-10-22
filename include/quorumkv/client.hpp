#pragma once

#include <chrono>
#include <map>
#include <memory>

#include "quorumkv/client_protocol.hpp"
#include "quorumkv/kv.hpp"
#include "quorumkv/net.hpp"

namespace quorumkv {

// Node i is reached at nodes[i - 1]. Leader hints use the same numbering.
struct ClientConfig {
  std::vector<net::Address> nodes;
  kv::ClientId id;
  std::chrono::milliseconds attempt_timeout{500};
  std::chrono::milliseconds operation_timeout{3000};
};

enum class Outcome : std::uint8_t {
  Ok,       // applied exactly once; `response` holds the result
  Fail,     // definitely had no effect: no node ever accepted it
  Unknown,  // may or may not have taken effect
};

struct Result {
  Outcome outcome{Outcome::Fail};
  kv::Response response{kv::Invalid{}};
};

// Blocking, single-threaded client with retry and leader discovery. Writes, and gets sent
// through the log, keep one session sequence across retries, so a retried request applies at
// most once. Gets through ReadIndex carry no session and are simply retried.
class Client {
 public:
  explicit Client(ClientConfig config);
  ~Client();
  Client(const Client&) = delete;
  auto operator=(const Client&) -> Client& = delete;
  Client(Client&&) = delete;
  auto operator=(Client&&) -> Client& = delete;

  auto Execute(kv::Operation operation) -> Result;
  auto Read(const std::string& key) -> Result;
  // Status of one node, or nullopt if it does not answer within the attempt timeout.
  auto Status(NodeId node) -> std::optional<NodeStatus>;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace quorumkv
