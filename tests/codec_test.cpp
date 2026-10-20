#include "quorumkv/codec.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace quorumkv {
namespace {

Message Wrap(MessageBody body) { return Message{1, 2, 3, 4, std::move(body)}; }

void SetLength(Bytes& frame) {
  const auto length = static_cast<std::uint32_t>(frame.size() - 4);
  for (unsigned i = 0; i < 4; ++i) {
    frame[i] = static_cast<std::uint8_t>(length >> (24 - 8 * i));
  }
}

std::vector<Message> Examples() {
  return {
      Wrap(RequestVote{5, 6}),
      Wrap(VoteResponse{true}),
      Wrap(AppendEntries{5, 6, 7, 8, {{9, 6, EntryKind::Command, {0, 128, 255}}}}),
      Wrap(AppendResponse{false, 5, 6, 7, 8}),
      Wrap(InstallSnapshot{5, 6, 7, true, {0, 128, 255}}),
      Wrap(SnapshotResponse{true, 5}),
  };
}

TEST(Codec, VoteMatchesIndependentBigEndianGoldenFrame) {
  const Bytes expected{0, 0, 0, 50, 1, 1,         // Payload length, version, tag.
                       0, 0, 0, 0,  0, 0, 0, 1,   // from
                       0, 0, 0, 0,  0, 0, 0, 2,   // to
                       0, 0, 0, 0,  0, 0, 0, 3,   // term
                       0, 0, 0, 0,  0, 0, 0, 4,   // rpc id
                       0, 0, 0, 0,  0, 0, 0, 5,   // last index
                       0, 0, 0, 0,  0, 0, 0, 6};  // last term
  const auto message = Wrap(RequestVote{5, 6});
  EXPECT_EQ(Encode(message), expected);
  EXPECT_EQ(Decode(expected), message);
}

TEST(Codec, AllKindsRoundTripAndRejectEveryTruncatedPrefix) {
  for (const auto& message : Examples()) {
    SCOPED_TRACE(message.body.index());
    const auto frame = Encode(message);
    EXPECT_EQ(Decode(frame), message);
    for (std::size_t length = 0; length < frame.size(); ++length) {
      SCOPED_TRACE(length);
      EXPECT_THROW(Decode(std::span(frame).first(length)), CodecError);
      // Repairing the header also exercises truncation inside each field.
      if (length >= 4) {
        Bytes truncated(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(length));
        SetLength(truncated);
        EXPECT_THROW(Decode(truncated), CodecError);
      }
    }
    auto trailing = frame;
    trailing.push_back(0);
    EXPECT_THROW(Decode(trailing), CodecError);
    SetLength(trailing);
    EXPECT_THROW(Decode(trailing), CodecError);
  }
}

TEST(Codec, RejectsUnknownVersionAndTag) {
  auto frame = Encode(Wrap(VoteResponse{false}));
  frame[4] = 2;
  EXPECT_THROW(Decode(frame), CodecError);
  frame[4] = 1;
  frame[5] = 0;
  EXPECT_THROW(Decode(frame), CodecError);
  frame[5] = 255;
  EXPECT_THROW(Decode(frame), CodecError);
}

TEST(Codec, RejectsNoncanonicalBooleansAndEntryKinds) {
  for (const auto& [message, offset] :
       std::vector<std::pair<Message, std::size_t>>{{Wrap(VoteResponse{true}), 38},
                                                    {Wrap(AppendResponse{}), 38},
                                                    {Wrap(AppendResponse{}), 47},
                                                    {Wrap(InstallSnapshot{}), 62},
                                                    {Wrap(SnapshotResponse{}), 38}}) {
    auto frame = Encode(message);
    frame[offset] = 2;
    EXPECT_THROW(Decode(frame), CodecError);
  }
  auto message = Wrap(AppendEntries{0, 0, 0, 0, {{1, 1, EntryKind::NoOp, {}}}});
  auto frame = Encode(message);
  frame[90] = 2;
  EXPECT_THROW(Decode(frame), CodecError);
  std::get<AppendEntries>(message.body).entries[0].kind = static_cast<EntryKind>(255);
  EXPECT_THROW(Encode(message), CodecError);
}

TEST(Codec, PreservesEmptyFieldsAndFullWidthIntegers) {
  const auto max = std::numeric_limits<std::uint64_t>::max();
  const Message message{max, max, max, max, AppendResponse{true, max, max, max, max}};
  EXPECT_EQ(Decode(Encode(message)), message);
  for (const auto& body : std::vector<MessageBody>{
           RequestVote{}, VoteResponse{}, AppendEntries{}, AppendResponse{},
           AppendResponse{false, 0, Term{0}, 0, 0}, InstallSnapshot{}, SnapshotResponse{}}) {
    const auto empty = Wrap(body);
    EXPECT_EQ(Decode(Encode(empty)), empty);
  }
}

TEST(Codec, BoundsBlobAllocationBeforeReading) {
  auto message = Wrap(InstallSnapshot{0, 0, 0, true, Bytes(kMaxBlobBytes, 255)});
  EXPECT_EQ(Decode(Encode(message)), message);
  std::get<InstallSnapshot>(message.body).data.push_back(0);
  EXPECT_THROW(Encode(message), CodecError);
  auto frame = Encode(Wrap(InstallSnapshot{}));
  for (std::size_t i = 63; i < 67; ++i) {
    frame[i] = 255;
  }
  EXPECT_THROW(Decode(frame), CodecError);
  frame[63] = frame[64] = frame[65] = 0;
  frame[66] = 1;
  EXPECT_THROW(Decode(frame), CodecError);
}

TEST(Codec, BoundsEntryAllocationBeforeReading) {
  auto message = Wrap(AppendEntries{0, 0, 0, 0, std::vector<LogEntry>(kMaxEntries)});
  EXPECT_EQ(Decode(Encode(message)), message);
  std::get<AppendEntries>(message.body).entries.emplace_back();
  EXPECT_THROW(Encode(message), CodecError);
  auto frame = Encode(Wrap(AppendEntries{}));
  for (std::size_t i = 70; i < 74; ++i) {
    frame[i] = 255;
  }
  EXPECT_THROW(Decode(frame), CodecError);
  frame[70] = frame[71] = frame[72] = 0;
  frame[73] = 1;
  EXPECT_THROW(Decode(frame), CodecError);
}

TEST(Codec, EnforcesWholeFrameBudget) {
  AppendEntries body;
  for (unsigned i = 0; i < 4; ++i) {
    body.entries.push_back(LogEntry{1, 1, EntryKind::Command, Bytes(kMaxBlobBytes, 0)});
  }
  // Four entry headers plus the frame header use 158 bytes.
  body.entries.back().command.resize(kMaxBlobBytes - 158);
  auto message = Wrap(body);
  const auto exact = Encode(message);
  ASSERT_EQ(exact.size(), kMaxFrameBytes);
  EXPECT_EQ(Decode(exact), message);
  std::get<AppendEntries>(message.body).entries.back().command.push_back(0);
  EXPECT_THROW(Encode(message), CodecError);
  auto oversized = exact;
  oversized.push_back(0);
  SetLength(oversized);
  EXPECT_THROW(Decode(oversized), CodecError);
}

}  // namespace
}  // namespace quorumkv
