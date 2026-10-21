#include <type_traits>
#include <utility>

#include "quorumkv/codec.hpp"
#include "quorumkv/kv.hpp"

namespace quorumkv::kv {
namespace {

constexpr std::uint8_t kVersion = 1;

enum class Kind : std::uint8_t { Request = 1, Read = 2, Response = 3, Snapshot = 4 };
enum class OpTag : std::uint8_t { Put = 1, Get = 2, Append = 3, Cas = 4 };
enum class ResultTag : std::uint8_t {
  WriteOk = 1,
  Value = 2,
  CasResult = 3,
  StaleSequence = 4,
  Invalid = 5
};

class Writer {
 public:
  explicit Writer(Kind kind) {
    bytes_.push_back(kVersion);
    bytes_.push_back(static_cast<std::uint8_t>(kind));
  }
  void U8(std::uint8_t value) { bytes_.push_back(value); }
  void Bool(bool value) { U8(value ? 1 : 0); }
  void U64(std::uint64_t value) {
    for (unsigned shift = 64; shift > 0; shift -= 8) {
      bytes_.push_back(static_cast<std::uint8_t>(value >> (shift - 8)));
    }
  }
  void Blob(const std::string& value) {
    if (value.size() > kMaxBlobBytes) {
      throw CodecError("kv blob exceeds limit");
    }
    const auto size = static_cast<std::uint32_t>(value.size());
    for (unsigned shift = 32; shift > 0; shift -= 8) {
      bytes_.push_back(static_cast<std::uint8_t>(size >> (shift - 8)));
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  auto Finish() && -> Bytes { return std::move(bytes_); }

 private:
  Bytes bytes_;
};

class Reader {
 public:
  Reader(std::span<const std::uint8_t> bytes, Kind kind) : bytes_(bytes) {
    if (U8() != kVersion) {
      throw CodecError("unsupported kv version");
    }
    if (U8() != static_cast<std::uint8_t>(kind)) {
      throw CodecError("unexpected kv message kind");
    }
  }
  auto U8() -> std::uint8_t {
    Need(1);
    return bytes_[cursor_++];
  }
  auto Bool() -> bool {
    const auto value = U8();
    if (value > 1) {
      throw CodecError("invalid kv boolean");
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
  auto Blob() -> std::string {
    Need(4);
    std::uint32_t size{};
    for (unsigned index = 0; index < 4; ++index) {
      size = (size << 8) | bytes_[cursor_++];
    }
    if (size > kMaxBlobBytes) {
      throw CodecError("invalid kv blob length");
    }
    Need(size);
    std::string value(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                      bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_ + size));
    cursor_ += size;
    return value;
  }
  void End() const {
    if (cursor_ != bytes_.size()) {
      throw CodecError("trailing kv bytes");
    }
  }

 private:
  void Need(std::size_t count) const {
    if (count > bytes_.size() - cursor_) {
      throw CodecError("truncated kv message");
    }
  }
  std::span<const std::uint8_t> bytes_;
  std::size_t cursor_{};
};

void WriteResponse(Writer& writer, const Response& response) {
  std::visit(
      [&](const auto& result) {
        using T = std::decay_t<decltype(result)>;
        if constexpr (std::is_same_v<T, WriteOk>) {
          writer.U8(static_cast<std::uint8_t>(ResultTag::WriteOk));
        } else if constexpr (std::is_same_v<T, Value>) {
          writer.U8(static_cast<std::uint8_t>(ResultTag::Value));
          writer.Bool(result.found);
          writer.Blob(result.value);
        } else if constexpr (std::is_same_v<T, CasResult>) {
          writer.U8(static_cast<std::uint8_t>(ResultTag::CasResult));
          writer.Bool(result.exchanged);
          writer.Bool(result.found);
          writer.Blob(result.previous);
        } else if constexpr (std::is_same_v<T, StaleSequence>) {
          writer.U8(static_cast<std::uint8_t>(ResultTag::StaleSequence));
        } else {
          static_assert(std::is_same_v<T, Invalid>);
          writer.U8(static_cast<std::uint8_t>(ResultTag::Invalid));
        }
      },
      response);
}

auto ReadResponse(Reader& reader) -> Response {
  switch (static_cast<ResultTag>(reader.U8())) {
    case ResultTag::WriteOk:
      return WriteOk{};
    case ResultTag::Value: {
      Value value;
      value.found = reader.Bool();
      value.value = reader.Blob();
      if (!value.found && !value.value.empty()) {
        throw CodecError("missing value carries bytes");
      }
      return value;
    }
    case ResultTag::CasResult: {
      CasResult result;
      result.exchanged = reader.Bool();
      result.found = reader.Bool();
      result.previous = reader.Blob();
      if ((!result.found && !result.previous.empty()) || (result.exchanged && !result.found)) {
        throw CodecError("inconsistent CAS result");
      }
      return result;
    }
    case ResultTag::StaleSequence:
      return StaleSequence{};
    case ResultTag::Invalid:
      return Invalid{};
  }
  throw CodecError("unknown kv result tag");
}

void Mix(std::uint64_t& digest, std::span<const std::uint8_t> bytes) {
  for (const auto byte : bytes) {
    digest = (digest ^ byte) * 1099511628211ULL;
  }
  // Separate adjacent fields so concatenations cannot collide trivially.
  digest = (digest ^ 0xffU) * 1099511628211ULL;
}

}  // namespace

auto EncodeRequest(const Request& request) -> Bytes {
  Writer writer(Kind::Request);
  writer.U64(request.client.high);
  writer.U64(request.client.low);
  writer.U64(request.sequence);
  std::visit(
      [&](const auto& operation) {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, Put>) {
          writer.U8(static_cast<std::uint8_t>(OpTag::Put));
          writer.Blob(operation.key);
          writer.Blob(operation.value);
        } else if constexpr (std::is_same_v<T, Get>) {
          writer.U8(static_cast<std::uint8_t>(OpTag::Get));
          writer.Blob(operation.key);
        } else if constexpr (std::is_same_v<T, Append>) {
          writer.U8(static_cast<std::uint8_t>(OpTag::Append));
          writer.Blob(operation.key);
          writer.Blob(operation.suffix);
        } else {
          static_assert(std::is_same_v<T, Cas>);
          writer.U8(static_cast<std::uint8_t>(OpTag::Cas));
          writer.Blob(operation.key);
          writer.Blob(operation.expected);
          writer.Blob(operation.replacement);
        }
      },
      request.operation);
  auto bytes = std::move(writer).Finish();
  if (bytes.size() > kMaxBlobBytes) {
    throw CodecError("kv request exceeds Raft command limit");
  }
  return bytes;
}

auto DecodeRequest(std::span<const std::uint8_t> bytes) -> Request {
  Reader reader(bytes, Kind::Request);
  Request request;
  request.client.high = reader.U64();
  request.client.low = reader.U64();
  request.sequence = reader.U64();
  if (request.sequence == 0) {
    throw CodecError("client sequences start at one");
  }
  switch (static_cast<OpTag>(reader.U8())) {
    case OpTag::Put: {
      auto key = reader.Blob();
      request.operation = Put{.key = std::move(key), .value = reader.Blob()};
      break;
    }
    case OpTag::Get:
      request.operation = Get{.key = reader.Blob()};
      break;
    case OpTag::Append: {
      auto key = reader.Blob();
      request.operation = Append{.key = std::move(key), .suffix = reader.Blob()};
      break;
    }
    case OpTag::Cas: {
      auto key = reader.Blob();
      auto expected = reader.Blob();
      request.operation =
          Cas{.key = std::move(key), .expected = std::move(expected), .replacement = reader.Blob()};
      break;
    }
    default:
      throw CodecError("unknown kv operation tag");
  }
  reader.End();
  return request;
}

auto EncodeRead(const std::string& key) -> Bytes {
  Writer writer(Kind::Read);
  writer.Blob(key);
  return std::move(writer).Finish();
}

auto DecodeRead(std::span<const std::uint8_t> bytes) -> std::string {
  Reader reader(bytes, Kind::Read);
  auto key = reader.Blob();
  reader.End();
  return key;
}

auto EncodeResponse(const Response& response) -> Bytes {
  Writer writer(Kind::Response);
  WriteResponse(writer, response);
  return std::move(writer).Finish();
}

auto DecodeResponse(std::span<const std::uint8_t> bytes) -> Response {
  Reader reader(bytes, Kind::Response);
  auto response = ReadResponse(reader);
  reader.End();
  return response;
}

auto Store::Execute(const Operation& operation) -> Response {
  return std::visit(
      [&](const auto& op) -> Response {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, Put>) {
          data_[op.key] = op.value;
          return WriteOk{};
        } else if constexpr (std::is_same_v<T, Get>) {
          const auto found = data_.find(op.key);
          return found == data_.end() ? Value{} : Value{.found = true, .value = found->second};
        } else if constexpr (std::is_same_v<T, Append>) {
          data_[op.key] += op.suffix;
          return WriteOk{};
        } else {
          static_assert(std::is_same_v<T, Cas>);
          const auto found = data_.find(op.key);
          if (found == data_.end()) {
            return CasResult{.exchanged = false, .found = false, .previous = {}};
          }
          CasResult result{
              .exchanged = found->second == op.expected, .found = true, .previous = found->second};
          if (result.exchanged) {
            found->second = op.replacement;
          }
          return result;
        }
      },
      operation);
}

auto Store::Apply(std::span<const std::uint8_t> command) -> Bytes {
  Bytes response;
  try {
    const auto request = DecodeRequest(command);
    auto& session = sessions_[request.client];
    if (request.sequence == session.sequence) {
      // A retry of the latest applied command returns its original result.
      response = session.response;
    } else if (request.sequence < session.sequence) {
      response = EncodeResponse(StaleSequence{});
    } else {
      response = EncodeResponse(Execute(request.operation));
      session = Session{.sequence = request.sequence, .response = response};
    }
  } catch (const CodecError&) {
    response = EncodeResponse(Invalid{});
  }
  Mix(digest_, command);
  Mix(digest_, response);
  return response;
}

auto Store::Read(std::span<const std::uint8_t> query) const -> Bytes {
  const auto key = DecodeRead(query);
  const auto found = data_.find(key);
  return EncodeResponse(found == data_.end() ? Value{}
                                             : Value{.found = true, .value = found->second});
}

auto Store::Snapshot() const -> Bytes {
  Writer writer(Kind::Snapshot);
  writer.U64(digest_);
  writer.U64(data_.size());
  for (const auto& [key, value] : data_) {
    writer.Blob(key);
    writer.Blob(value);
  }
  writer.U64(sessions_.size());
  for (const auto& [client, session] : sessions_) {
    writer.U64(client.high);
    writer.U64(client.low);
    writer.U64(session.sequence);
    writer.Blob(std::string(session.response.begin(), session.response.end()));
  }
  return std::move(writer).Finish();
}

void Store::Restore(std::span<const std::uint8_t> snapshot) {
  Reader reader(snapshot, Kind::Snapshot);
  const auto digest = reader.U64();
  std::map<std::string, std::string> data;
  std::map<ClientId, Session> sessions;
  for (auto count = reader.U64(); count > 0; --count) {
    auto key = reader.Blob();
    if (!data.empty() && key <= data.rbegin()->first) {
      throw CodecError("snapshot keys must be strictly increasing");
    }
    data.emplace_hint(data.end(), std::move(key), reader.Blob());
  }
  for (auto count = reader.U64(); count > 0; --count) {
    const ClientId client{.high = reader.U64(), .low = reader.U64()};
    if (!sessions.empty() && !(sessions.rbegin()->first < client)) {
      throw CodecError("snapshot sessions must be strictly increasing");
    }
    const auto sequence = reader.U64();
    const auto response = reader.Blob();
    static_cast<void>(DecodeResponse(
        std::span(reinterpret_cast<const std::uint8_t*>(response.data()), response.size())));
    sessions.emplace_hint(
        sessions.end(), client,
        Session{.sequence = sequence, .response = Bytes(response.begin(), response.end())});
  }
  reader.End();
  // Replace state only after the whole snapshot validates.
  data_ = std::move(data);
  sessions_ = std::move(sessions);
  digest_ = digest;
}

}  // namespace quorumkv::kv
