#include "quorumkv/client_protocol.hpp"

#include <utility>

#include "quorumkv/codec.hpp"

namespace quorumkv {
namespace {

class Writer {
 public:
  explicit Writer(std::uint8_t tag) : bytes_(4) {
    bytes_.push_back(kWireVersion);
    bytes_.push_back(tag);
  }
  void U8(std::uint8_t value) { bytes_.push_back(value); }
  void U64(std::uint64_t value) {
    for (unsigned shift = 64; shift > 0; shift -= 8) {
      bytes_.push_back(static_cast<std::uint8_t>(value >> (shift - 8)));
    }
  }
  void Blob(std::span<const std::uint8_t> value) {
    if (value.size() > kMaxBlobBytes) {
      throw CodecError("client payload exceeds limit");
    }
    const auto size = static_cast<std::uint32_t>(value.size());
    for (unsigned shift = 32; shift > 0; shift -= 8) {
      bytes_.push_back(static_cast<std::uint8_t>(size >> (shift - 8)));
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  auto Frame() && -> Bytes {
    const auto length = static_cast<std::uint32_t>(bytes_.size() - 4);
    for (unsigned index = 0; index < 4; ++index) {
      bytes_[index] = static_cast<std::uint8_t>(length >> (24 - 8 * index));
    }
    return std::move(bytes_);
  }
  auto Payload() && -> Bytes {
    // Status payloads are nested in a response; they carry version and tag but no prefix.
    bytes_.erase(bytes_.begin(), bytes_.begin() + 4);
    return std::move(bytes_);
  }

 private:
  Bytes bytes_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  auto U8() -> std::uint8_t {
    Need(1);
    return bytes_[cursor_++];
  }
  auto Bool() -> bool {
    const auto value = U8();
    if (value > 1) {
      throw CodecError("invalid client boolean");
    }
    return value == 1;
  }
  auto U64() -> std::uint64_t {
    Need(8);
    std::uint64_t value{};
    for (unsigned index = 0; index < 8; ++index) {
      value = (value << 8) | bytes_[cursor_++];
    }
    return value;
  }
  auto Blob() -> Bytes {
    Need(4);
    std::uint32_t size{};
    for (unsigned index = 0; index < 4; ++index) {
      size = (size << 8) | bytes_[cursor_++];
    }
    if (size > kMaxBlobBytes) {
      throw CodecError("invalid client payload length");
    }
    Need(size);
    Bytes value(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_ + size));
    cursor_ += size;
    return value;
  }
  void End() const {
    if (cursor_ != bytes_.size()) {
      throw CodecError("trailing client bytes");
    }
  }

 private:
  void Need(std::size_t count) const {
    if (count > bytes_.size() - cursor_) {
      throw CodecError("truncated client frame");
    }
  }
  std::span<const std::uint8_t> bytes_;
  std::size_t cursor_{};
};

// Validates the length prefix, version, and tag; returns a reader over the body.
auto OpenFrame(std::span<const std::uint8_t> frame, std::uint8_t tag) -> Reader {
  if (frame.size() > kMaxFrameBytes || frame.size() < 6) {
    throw CodecError("invalid client frame size");
  }
  Reader prefix(frame.first(4));
  std::uint32_t length = 0;
  for (unsigned index = 0; index < 4; ++index) {
    length = (length << 8) | prefix.U8();
  }
  if (length != frame.size() - 4) {
    throw CodecError("client frame length mismatch");
  }
  if (frame[4] != kWireVersion) {
    throw CodecError("unsupported client wire version");
  }
  if (frame[5] != tag) {
    throw CodecError("unexpected client frame tag");
  }
  return Reader(frame.subspan(6));
}

constexpr std::uint8_t kStatusTag = 0x53;

}  // namespace

auto FrameTag(std::span<const std::uint8_t> frame) -> std::optional<std::uint8_t> {
  return frame.size() >= 6 ? std::optional<std::uint8_t>{frame[5]} : std::nullopt;
}

auto EncodeClientRequest(const ClientRequest& request) -> Bytes {
  if (request.kind != ClientKind::Command && request.kind != ClientKind::Read &&
      request.kind != ClientKind::Status) {
    throw CodecError("invalid client request kind");
  }
  Writer writer(kClientRequestTag);
  writer.U64(request.request_id);
  writer.U8(static_cast<std::uint8_t>(request.kind));
  writer.Blob(request.payload);
  return std::move(writer).Frame();
}

auto DecodeClientRequest(std::span<const std::uint8_t> frame) -> ClientRequest {
  auto reader = OpenFrame(frame, kClientRequestTag);
  ClientRequest request;
  request.request_id = reader.U64();
  const auto kind = reader.U8();
  if (kind < 1 || kind > 3) {
    throw CodecError("unknown client request kind");
  }
  request.kind = static_cast<ClientKind>(kind);
  request.payload = reader.Blob();
  reader.End();
  return request;
}

auto EncodeClientResponse(const ClientResponse& response) -> Bytes {
  if (static_cast<std::uint8_t>(response.status) > 2) {
    throw CodecError("invalid client status");
  }
  Writer writer(kClientResponseTag);
  writer.U64(response.request_id);
  writer.U8(static_cast<std::uint8_t>(response.status));
  writer.U8(response.leader_hint ? 1 : 0);
  writer.U64(response.leader_hint.value_or(0));
  writer.Blob(response.payload);
  return std::move(writer).Frame();
}

auto DecodeClientResponse(std::span<const std::uint8_t> frame) -> ClientResponse {
  auto reader = OpenFrame(frame, kClientResponseTag);
  ClientResponse response;
  response.request_id = reader.U64();
  const auto status = reader.U8();
  if (status > 2) {
    throw CodecError("unknown client status");
  }
  response.status = static_cast<ClientStatus>(status);
  const bool has_hint = reader.Bool();
  const auto hint = reader.U64();
  if (!has_hint && hint != 0) {
    throw CodecError("absent leader hint carries a value");
  }
  if (has_hint) {
    response.leader_hint = hint;
  }
  response.payload = reader.Blob();
  reader.End();
  return response;
}

auto EncodeNodeStatus(const NodeStatus& status) -> Bytes {
  Writer writer(kStatusTag);
  writer.U64(status.id);
  writer.U8(static_cast<std::uint8_t>(status.role));
  writer.U64(status.term);
  writer.U8(status.leader ? 1 : 0);
  writer.U64(status.leader.value_or(0));
  writer.U64(status.commit_index);
  writer.U64(status.last_applied);
  writer.U64(status.snapshot_index);
  return std::move(writer).Payload();
}

auto DecodeNodeStatus(std::span<const std::uint8_t> bytes) -> NodeStatus {
  Reader reader(bytes);
  if (reader.U8() != kWireVersion || reader.U8() != kStatusTag) {
    throw CodecError("invalid node status header");
  }
  NodeStatus status;
  status.id = reader.U64();
  const auto role = reader.U8();
  if (role > 2) {
    throw CodecError("invalid node role");
  }
  status.role = static_cast<Role>(role);
  status.term = reader.U64();
  const bool has_leader = reader.Bool();
  const auto leader = reader.U64();
  if (has_leader) {
    status.leader = leader;
  } else if (leader != 0) {
    throw CodecError("absent leader carries a value");
  }
  status.commit_index = reader.U64();
  status.last_applied = reader.U64();
  status.snapshot_index = reader.U64();
  reader.End();
  return status;
}

}  // namespace quorumkv
