#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "quorumkv/types.hpp"

namespace quorumkv::net {

// Process-wide socket library setup. Required once before any other call on Windows.
void Initialize();

struct Address {
  std::string host;
  std::uint16_t port{};
  auto operator==(const Address&) const -> bool = default;
};
// Parses "host:port". Throws std::invalid_argument.
auto ParseAddress(const std::string& text) -> Address;

// Owns one non-blocking socket.
class Socket {
 public:
  Socket() = default;
  explicit Socket(std::intptr_t handle) : handle_(handle) {}
  ~Socket();
  Socket(const Socket&) = delete;
  auto operator=(const Socket&) -> Socket& = delete;
  Socket(Socket&& other) noexcept;
  auto operator=(Socket&& other) noexcept -> Socket&;
  [[nodiscard]] auto valid() const -> bool { return handle_ != kInvalid; }
  [[nodiscard]] auto handle() const -> std::intptr_t { return handle_; }
  void Close();

  static constexpr std::intptr_t kInvalid = -1;

 private:
  std::intptr_t handle_{kInvalid};
};

auto Listen(const Address& address) -> Socket;
// The port a socket is bound to, for listeners created on port 0.
auto LocalPort(const Socket& socket) -> std::uint16_t;
// Returns an invalid socket when no connection is pending.
auto Accept(const Socket& listener) -> Socket;
// Starts a non-blocking connect. The socket becomes writable once connected.
auto Connect(const Address& address) -> Socket;
// True once a pending connect has completed successfully.
auto Connected(const Socket& socket) -> bool;

enum class IoResult : std::uint8_t { Progress, WouldBlock, Closed };
// Appends whatever is available to `buffer`.
auto Receive(const Socket& socket, Bytes& buffer) -> IoResult;
// Sends from the front of `buffer` and erases what was sent.
auto SendSome(const Socket& socket, Bytes& buffer) -> IoResult;

struct PollEntry {
  const Socket* socket{};
  bool want_read{};
  bool want_write{};
  bool readable{};
  bool writable{};
  bool failed{};
};
// Waits up to `timeout_ms` for any entry to become ready.
void Poll(std::vector<PollEntry>& entries, int timeout_ms);

// Splits a byte stream into complete length-prefixed frames, bounded by kMaxFrameBytes.
class FrameReader {
 public:
  void Append(std::span<const std::uint8_t> bytes);
  // Throws CodecError when a declared length exceeds the frame limit.
  auto Next() -> std::optional<Bytes>;

 private:
  Bytes buffer_;
};

}  // namespace quorumkv::net
