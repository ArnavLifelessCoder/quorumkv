#include "quorumkv/codec.hpp"

#include <type_traits>
#include <utility>

namespace quorumkv {
namespace {

enum class Tag : std::uint8_t {
  Vote = 1,
  VoteReply = 2,
  Append = 3,
  AppendReply = 4,
  Snapshot = 5,
  SnapshotReply = 6
};

class Writer {
 public:
  void U8(std::uint8_t value) {
    Check(1);
    data_.push_back(value);
  }

  void U32(std::uint32_t value) { Integer<4>(value); }
  void U64(std::uint64_t value) { Integer<8>(value); }
  void Bool(bool value) { U8(value ? 1 : 0); }

  void Blob(const Bytes& bytes) {
    if (bytes.size() > kMaxBlobBytes) {
      throw CodecError("blob exceeds limit");
    }
    U32(static_cast<std::uint32_t>(bytes.size()));
    Check(bytes.size());
    data_.insert(data_.end(), bytes.begin(), bytes.end());
  }

  auto Finish() && -> Bytes {
    const auto length = static_cast<std::uint32_t>(data_.size() - 4);
    for (unsigned i = 0; i < 4; ++i) {
      data_[i] = static_cast<std::uint8_t>(length >> (24 - 8 * i));
    }
    return std::move(data_);
  }

 private:
  void Check(std::size_t amount) const {
    if (amount > kMaxFrameBytes - data_.size()) {
      throw CodecError("frame exceeds limit");
    }
  }

  template <unsigned Width>
  void Integer(std::uint64_t value) {
    Check(Width);
    for (unsigned i = Width; i > 0; --i) {
      data_.push_back(static_cast<std::uint8_t>(value >> (8 * (i - 1))));
    }
  }

  Bytes data_ = Bytes(4);
};

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

  [[nodiscard]] auto Remaining() const -> std::size_t { return data_.size() - cursor_; }
  auto U8() -> std::uint8_t { return static_cast<std::uint8_t>(Integer(1)); }
  auto U32() -> std::uint32_t { return static_cast<std::uint32_t>(Integer(4)); }
  auto U64() -> std::uint64_t { return Integer(8); }

  auto Bool() -> bool {
    const auto value = U8();
    if (value > 1) {
      throw CodecError("invalid boolean");
    }
    return value == 1;
  }

  auto Blob() -> Bytes {
    const auto size = U32();
    if (size > kMaxBlobBytes || size > Remaining()) {
      throw CodecError("invalid blob length");
    }
    const auto bytes = data_.subspan(cursor_, size);
    cursor_ += size;
    return {bytes.begin(), bytes.end()};
  }

 private:
  auto Integer(unsigned width) -> std::uint64_t {
    if (width > Remaining()) {
      throw CodecError("truncated frame");
    }
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) {
      result = (result << 8) | data_[cursor_++];
    }
    return result;
  }

  std::span<const std::uint8_t> data_;
  std::size_t cursor_{};
};

template <class Body>
constexpr auto BodyTag() -> Tag {
  if constexpr (std::is_same_v<Body, RequestVote>) {
    return Tag::Vote;
  } else if constexpr (std::is_same_v<Body, VoteResponse>) {
    return Tag::VoteReply;
  } else if constexpr (std::is_same_v<Body, AppendEntries>) {
    return Tag::Append;
  } else if constexpr (std::is_same_v<Body, AppendResponse>) {
    return Tag::AppendReply;
  } else if constexpr (std::is_same_v<Body, InstallSnapshot>) {
    return Tag::Snapshot;
  } else {
    static_assert(std::is_same_v<Body, SnapshotResponse>);
    return Tag::SnapshotReply;
  }
}

void WriteBody(Writer& writer, const RequestVote& body) {
  writer.U64(body.last_log_index);
  writer.U64(body.last_log_term);
}

void WriteBody(Writer& writer, const VoteResponse& body) { writer.Bool(body.granted); }

void WriteBody(Writer& writer, const AppendEntries& body) {
  if (body.entries.size() > kMaxEntries) {
    throw CodecError("entry count exceeds limit");
  }
  writer.U64(body.prev_log_index);
  writer.U64(body.prev_log_term);
  writer.U64(body.leader_commit);
  writer.U64(body.read_context);
  writer.U32(static_cast<std::uint32_t>(body.entries.size()));
  for (const auto& entry : body.entries) {
    if (entry.kind != EntryKind::NoOp && entry.kind != EntryKind::Command) {
      throw CodecError("invalid entry kind");
    }
    writer.U64(entry.index);
    writer.U64(entry.term);
    writer.U8(static_cast<std::uint8_t>(entry.kind));
    writer.Blob(entry.command);
  }
}

void WriteBody(Writer& writer, const AppendResponse& body) {
  writer.Bool(body.success);
  writer.U64(body.match_index);
  writer.Bool(body.conflict_term.has_value());
  if (body.conflict_term) {
    writer.U64(*body.conflict_term);
  }
  writer.U64(body.conflict_index);
  writer.U64(body.read_context);
}

void WriteBody(Writer& writer, const InstallSnapshot& body) {
  writer.U64(body.last_included_index);
  writer.U64(body.last_included_term);
  writer.U64(body.offset);
  writer.Bool(body.done);
  writer.Blob(body.data);
}

void WriteBody(Writer& writer, const SnapshotResponse& body) {
  writer.Bool(body.success);
  writer.U64(body.next_offset);
}

auto ReadBody(Reader& reader, Tag tag) -> MessageBody {
  switch (tag) {
    case Tag::Vote: {
      RequestVote body;
      body.last_log_index = reader.U64();
      body.last_log_term = reader.U64();
      return body;
    }
    case Tag::VoteReply:
      return VoteResponse{reader.Bool()};
    case Tag::Append: {
      AppendEntries body;
      body.prev_log_index = reader.U64();
      body.prev_log_term = reader.U64();
      body.leader_commit = reader.U64();
      body.read_context = reader.U64();
      const auto count = reader.U32();
      // Even an empty entry consumes 21 bytes. Check before allocation.
      if (count > kMaxEntries || count > reader.Remaining() / 21) {
        throw CodecError("invalid entry count");
      }
      body.entries.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        LogEntry entry;
        entry.index = reader.U64();
        entry.term = reader.U64();
        entry.kind = static_cast<EntryKind>(reader.U8());
        if (entry.kind != EntryKind::NoOp && entry.kind != EntryKind::Command) {
          throw CodecError("invalid entry kind");
        }
        entry.command = reader.Blob();
        body.entries.push_back(std::move(entry));
      }
      return body;
    }
    case Tag::AppendReply: {
      AppendResponse body;
      body.success = reader.Bool();
      body.match_index = reader.U64();
      if (reader.Bool()) {
        body.conflict_term = reader.U64();
      }
      body.conflict_index = reader.U64();
      body.read_context = reader.U64();
      return body;
    }
    case Tag::Snapshot: {
      InstallSnapshot body;
      body.last_included_index = reader.U64();
      body.last_included_term = reader.U64();
      body.offset = reader.U64();
      body.done = reader.Bool();
      body.data = reader.Blob();
      return body;
    }
    case Tag::SnapshotReply: {
      SnapshotResponse body;
      body.success = reader.Bool();
      body.next_offset = reader.U64();
      return body;
    }
  }
  throw CodecError("unknown message tag");
}

}  // namespace

auto Encode(const Message& message) -> Bytes {
  Writer writer;
  writer.U8(kWireVersion);
  std::visit(
      [&](const auto& body) {
        writer.U8(static_cast<std::uint8_t>(BodyTag<std::decay_t<decltype(body)>>()));
        writer.U64(message.from);
        writer.U64(message.to);
        writer.U64(message.term);
        writer.U64(message.rpc_id);
        WriteBody(writer, body);
      },
      message.body);
  return std::move(writer).Finish();
}

auto Decode(std::span<const std::uint8_t> frame) -> Message {
  if (frame.size() > kMaxFrameBytes) {
    throw CodecError("frame exceeds limit");
  }
  Reader reader(frame);
  const auto length = reader.U32();
  if (length != reader.Remaining()) {
    throw CodecError("frame length mismatch");
  }
  if (reader.U8() != kWireVersion) {
    throw CodecError("unsupported wire version");
  }
  const auto tag = static_cast<Tag>(reader.U8());
  Message message;
  message.from = reader.U64();
  message.to = reader.U64();
  message.term = reader.U64();
  message.rpc_id = reader.U64();
  message.body = ReadBody(reader, tag);
  if (reader.Remaining() != 0) {
    throw CodecError("trailing bytes");
  }
  return message;
}

}  // namespace quorumkv
