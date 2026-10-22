#include "quorumkv/client_protocol.hpp"

#include <gtest/gtest.h>

#include "quorumkv/codec.hpp"
#include "quorumkv/net.hpp"

namespace quorumkv {
namespace {

TEST(ClientProtocol, RequestsResponsesAndStatusRoundTrip) {
  for (const auto kind : {ClientKind::Command, ClientKind::Read, ClientKind::Status}) {
    const ClientRequest request{.request_id = ~0ULL, .kind = kind, .payload = {0, 255, 7}};
    EXPECT_EQ(DecodeClientRequest(EncodeClientRequest(request)), request);
  }
  for (const auto& response : std::vector<ClientResponse>{
           {.request_id = 1,
            .status = ClientStatus::Ok,
            .leader_hint = std::nullopt,
            .payload = {1}},
           {.request_id = 2, .status = ClientStatus::NotLeader, .leader_hint = 0, .payload = {}},
           {.request_id = 3, .status = ClientStatus::NotLeader, .leader_hint = 3, .payload = {}},
           {.request_id = 4,
            .status = ClientStatus::Retry,
            .leader_hint = std::nullopt,
            .payload = {}}}) {
    EXPECT_EQ(DecodeClientResponse(EncodeClientResponse(response)), response);
  }
  const NodeStatus status{.id = 2,
                          .role = Role::Leader,
                          .term = 9,
                          .leader = 2,
                          .commit_index = 40,
                          .last_applied = 39,
                          .snapshot_index = 32};
  EXPECT_EQ(DecodeNodeStatus(EncodeNodeStatus(status)), status);
}

TEST(ClientProtocol, RejectsTruncationWrongTagsAndNoncanonicalFields) {
  const auto frame =
      EncodeClientRequest({.request_id = 5, .kind = ClientKind::Read, .payload = {1, 2}});
  for (std::size_t length = 0; length < frame.size(); ++length) {
    EXPECT_THROW(static_cast<void>(DecodeClientRequest(std::span(frame).first(length))),
                 CodecError);
  }
  EXPECT_THROW(static_cast<void>(DecodeClientResponse(frame)), CodecError);
  auto kind = frame;
  kind[14] = 9;
  EXPECT_THROW(static_cast<void>(DecodeClientRequest(kind)), CodecError);
  auto response = EncodeClientResponse(
      {.request_id = 5, .status = ClientStatus::Ok, .leader_hint = std::nullopt, .payload = {}});
  response[15] = 1;  // An absent leader hint must carry zero.
  response[23] = 1;
  auto hint = response;
  hint[15] = 0;
  EXPECT_THROW(static_cast<void>(DecodeClientResponse(hint)), CodecError);
  auto status = response;
  status[14] = 3;
  EXPECT_THROW(static_cast<void>(DecodeClientResponse(status)), CodecError);
  // Peer frames are never mistaken for client frames.
  const auto peer = Encode({.from = 1, .to = 2, .term = 1, .rpc_id = 1, .body = VoteResponse{}});
  EXPECT_THROW(static_cast<void>(DecodeClientRequest(peer)), CodecError);
  EXPECT_EQ(FrameTag(peer), 2U);
  EXPECT_EQ(FrameTag(frame), kClientRequestTag);
}

TEST(FrameReader, SplitsArbitraryChunksAndBoundsDeclaredLengths) {
  const auto first =
      EncodeClientRequest({.request_id = 1, .kind = ClientKind::Status, .payload = {}});
  const auto second =
      EncodeClientRequest({.request_id = 2, .kind = ClientKind::Command, .payload = {9, 9}});
  Bytes stream = first;
  stream.insert(stream.end(), second.begin(), second.end());
  net::FrameReader reader;
  for (const auto byte : stream) {
    reader.Append(std::span(&byte, 1));
  }
  EXPECT_EQ(reader.Next(), first);
  EXPECT_EQ(reader.Next(), second);
  EXPECT_EQ(reader.Next(), std::nullopt);
  net::FrameReader huge;
  const Bytes prefix{0xff, 0xff, 0xff, 0xff};
  huge.Append(prefix);
  EXPECT_THROW(static_cast<void>(huge.Next()), CodecError);
}

TEST(NetAddress, ParsesHostAndPortAndRejectsGarbage) {
  EXPECT_EQ(net::ParseAddress("127.0.0.1:7100"), (net::Address{.host = "127.0.0.1", .port = 7100}));
  for (const auto* text : {"127.0.0.1", ":80", "host:0", "host:65536", "host:8x"}) {
    EXPECT_THROW(static_cast<void>(net::ParseAddress(text)), std::invalid_argument) << text;
  }
}

}  // namespace
}  // namespace quorumkv
