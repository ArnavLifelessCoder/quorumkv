#include "quorumkv/net.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "quorumkv/codec.hpp"

namespace quorumkv::net {
namespace {

#ifdef _WIN32
using Native = SOCKET;
auto LastError() -> int { return WSAGetLastError(); }
auto WouldBlock(int error) -> bool { return error == WSAEWOULDBLOCK; }
auto InProgress(int error) -> bool { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
void CloseNative(Native handle) { closesocket(handle); }
#else
using Native = int;
auto LastError() -> int { return errno; }
auto WouldBlock(int error) -> bool { return error == EAGAIN || error == EWOULDBLOCK; }
auto InProgress(int error) -> bool { return error == EINPROGRESS; }
void CloseNative(Native handle) { close(handle); }
#endif

auto ToNative(const Socket& socket) -> Native { return static_cast<Native>(socket.handle()); }

[[noreturn]] void Fail(const char* what) {
  throw std::system_error(LastError(), std::system_category(), what);
}

void Configure(Native handle) {
#ifdef _WIN32
  u_long nonblocking = 1;
  if (ioctlsocket(handle, FIONBIO, &nonblocking) != 0) {
    Fail("set non-blocking");
  }
#else
  const int flags = fcntl(handle, F_GETFL, 0);
  if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0) {
    Fail("set non-blocking");
  }
#endif
  // Raft heartbeats are small and latency-sensitive.
  const int enable = 1;
  setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enable),
             sizeof(enable));
}

auto Resolve(const Address& address) -> sockaddr_in {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(address.port);
  const auto host = address.host == "localhost" ? std::string("127.0.0.1") : address.host;
  if (inet_pton(AF_INET, host.c_str(), &result.sin_addr) != 1) {
    throw std::invalid_argument("only IPv4 literal addresses and localhost are supported");
  }
  return result;
}

auto NewSocket() -> Native {
  const auto handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
  if (handle == INVALID_SOCKET) {
#else
  if (handle < 0) {
#endif
    Fail("create socket");
  }
  return handle;
}

}  // namespace

void Initialize() {
#ifdef _WIN32
  static const bool started = [] {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
    return true;
  }();
  static_cast<void>(started);
#endif
}

auto ParseAddress(const std::string& text) -> Address {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0) {
    throw std::invalid_argument("address must be host:port");
  }
  unsigned port{};
  const auto* begin = text.data() + colon + 1;
  const auto* end = text.data() + text.size();
  const auto [last, error] = std::from_chars(begin, end, port);
  if (error != std::errc{} || last != end || port == 0 || port > 65535) {
    throw std::invalid_argument("invalid port in address");
  }
  return {.host = text.substr(0, colon), .port = static_cast<std::uint16_t>(port)};
}

Socket::~Socket() { Close(); }

Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, kInvalid)) {}

auto Socket::operator=(Socket&& other) noexcept -> Socket& {
  if (this != &other) {
    Close();
    handle_ = std::exchange(other.handle_, kInvalid);
  }
  return *this;
}

void Socket::Close() {
  if (handle_ != kInvalid) {
    CloseNative(static_cast<Native>(handle_));
    handle_ = kInvalid;
  }
}

auto Listen(const Address& address) -> Socket {
  Socket socket(static_cast<std::intptr_t>(NewSocket()));
  const auto handle = ToNative(socket);
  const int reuse = 1;
  setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
  const auto target = Resolve(address);
  if (bind(handle, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0) {
    Fail("bind");
  }
  if (listen(handle, SOMAXCONN) != 0) {
    Fail("listen");
  }
  Configure(handle);
  return socket;
}

auto LocalPort(const Socket& socket) -> std::uint16_t {
  sockaddr_in bound{};
#ifdef _WIN32
  int length = sizeof(bound);
#else
  socklen_t length = sizeof(bound);
#endif
  if (getsockname(ToNative(socket), reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    Fail("getsockname");
  }
  return ntohs(bound.sin_port);
}

auto Accept(const Socket& listener) -> Socket {
  const auto handle = accept(ToNative(listener), nullptr, nullptr);
#ifdef _WIN32
  if (handle == INVALID_SOCKET) {
#else
  if (handle < 0) {
#endif
    return {};
  }
  Configure(handle);
  return Socket(static_cast<std::intptr_t>(handle));
}

auto Connect(const Address& address) -> Socket {
  Socket socket(static_cast<std::intptr_t>(NewSocket()));
  const auto handle = ToNative(socket);
  Configure(handle);
  const auto target = Resolve(address);
  if (connect(handle, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0 &&
      !InProgress(LastError())) {
    return {};
  }
  return socket;
}

auto Connected(const Socket& socket) -> bool {
  int error = 0;
#ifdef _WIN32
  int length = sizeof(error);
#else
  socklen_t length = sizeof(error);
#endif
  if (getsockopt(ToNative(socket), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error),
                 &length) != 0) {
    return false;
  }
  return error == 0;
}

auto Receive(const Socket& socket, Bytes& buffer) -> IoResult {
  bool progress = false;
  for (;;) {
    std::array<std::uint8_t, 16384> chunk{};
    const auto count = recv(ToNative(socket), reinterpret_cast<char*>(chunk.data()),
                            static_cast<int>(chunk.size()), 0);
    if (count > 0) {
      buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + count);
      progress = true;
      continue;
    }
    if (count == 0) {
      return IoResult::Closed;
    }
    return WouldBlock(LastError()) ? (progress ? IoResult::Progress : IoResult::WouldBlock)
                                   : IoResult::Closed;
  }
}

auto SendSome(const Socket& socket, Bytes& buffer) -> IoResult {
  std::size_t sent = 0;
  while (sent < buffer.size()) {
    const auto chunk = std::min<std::size_t>(buffer.size() - sent, 1 << 20);
#ifdef _WIN32
    const auto count = send(ToNative(socket), reinterpret_cast<const char*>(buffer.data() + sent),
                            static_cast<int>(chunk), 0);
#else
    const auto count = send(ToNative(socket), buffer.data() + sent, chunk, MSG_NOSIGNAL);
#endif
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && WouldBlock(LastError())) {
      break;
    }
    return IoResult::Closed;
  }
  buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(sent));
  return sent > 0 ? IoResult::Progress : IoResult::WouldBlock;
}

void Poll(std::vector<PollEntry>& entries, int timeout_ms) {
#ifdef _WIN32
  std::vector<WSAPOLLFD> native(entries.size());
#else
  std::vector<pollfd> native(entries.size());
#endif
  for (std::size_t index = 0; index < entries.size(); ++index) {
    native[index].fd = ToNative(*entries[index].socket);
    native[index].events = static_cast<short>((entries[index].want_read ? POLLIN : 0) |
                                              (entries[index].want_write ? POLLOUT : 0));
  }
  if (native.empty()) {
    // WSAPoll rejects an empty set; there is nothing to wait for but time.
#ifdef _WIN32
    Sleep(static_cast<DWORD>(timeout_ms));
#else
    poll(nullptr, 0, timeout_ms);
#endif
    return;
  }
#ifdef _WIN32
  const auto ready = WSAPoll(native.data(), static_cast<ULONG>(native.size()), timeout_ms);
#else
  const auto ready = poll(native.data(), native.size(), timeout_ms);
#endif
  if (ready < 0) {
    Fail("poll");
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto events = native[index].revents;
    entries[index].readable = (events & (POLLIN | POLLHUP)) != 0;
    entries[index].writable = (events & POLLOUT) != 0;
    entries[index].failed = (events & (POLLERR | POLLNVAL)) != 0;
  }
}

void FrameReader::Append(std::span<const std::uint8_t> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

auto FrameReader::Next() -> std::optional<Bytes> {
  if (buffer_.size() < 4) {
    return std::nullopt;
  }
  std::uint32_t length = 0;
  for (unsigned index = 0; index < 4; ++index) {
    length = (length << 8) | buffer_[index];
  }
  // Bound the prefix before buffering, so a peer cannot force a huge allocation.
  if (length + std::size_t{4} > kMaxFrameBytes) {
    throw CodecError("incoming frame exceeds limit");
  }
  if (buffer_.size() < length + std::size_t{4}) {
    return std::nullopt;
  }
  const auto end = buffer_.begin() + static_cast<std::ptrdiff_t>(length) + 4;
  Bytes frame(buffer_.begin(), end);
  buffer_.erase(buffer_.begin(), end);
  return frame;
}

}  // namespace quorumkv::net
