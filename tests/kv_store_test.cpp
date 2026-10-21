#include <gtest/gtest.h>

#include "quorumkv/codec.hpp"
#include "quorumkv/kv.hpp"

namespace quorumkv::kv {
namespace {

constexpr ClientId kAlice{.high = 1, .low = 2};
constexpr ClientId kBob{.high = 1, .low = 3};

auto Request(ClientId client, std::uint64_t sequence, Operation operation) -> Bytes {
  return EncodeRequest({.client = client, .sequence = sequence, .operation = std::move(operation)});
}

auto Apply(Store& store, ClientId client, std::uint64_t sequence, Operation operation) -> Response {
  return DecodeResponse(store.Apply(Request(client, sequence, std::move(operation))));
}

auto ReadKey(const Store& store, const std::string& key) -> Response {
  return DecodeResponse(store.Read(EncodeRead(key)));
}

TEST(KvCodec, EveryRequestAndResponseRoundTripsWithBinaryData) {
  const std::string binary("\0\xff\x01", 3);
  for (const auto& operation : std::vector<Operation>{
           Put{.key = binary, .value = ""}, Get{.key = ""}, Append{.key = "k", .suffix = binary},
           Cas{.key = "k", .expected = "", .replacement = binary}}) {
    const kv::Request request{
        .client = {.high = ~0ULL, .low = 0}, .sequence = ~0ULL, .operation = operation};
    EXPECT_EQ(DecodeRequest(EncodeRequest(request)), request);
  }
  for (const auto& response :
       std::vector<Response>{WriteOk{}, Value{}, Value{.found = true, .value = binary},
                             CasResult{.exchanged = true, .found = true, .previous = binary},
                             CasResult{}, StaleSequence{}, Invalid{}}) {
    EXPECT_EQ(DecodeResponse(EncodeResponse(response)), response);
  }
  EXPECT_EQ(DecodeRead(EncodeRead(binary)), binary);
}

TEST(KvCodec, RejectsTruncationTrailingBytesUnknownTagsAndNoncanonicalFields) {
  const auto frame = Request(kAlice, 1, Cas{.key = "k", .expected = "a", .replacement = "b"});
  for (std::size_t length = 0; length < frame.size(); ++length) {
    EXPECT_THROW(DecodeRequest(std::span(frame).first(length)), CodecError);
  }
  auto trailing = frame;
  trailing.push_back(0);
  EXPECT_THROW(DecodeRequest(trailing), CodecError);
  auto version = frame;
  version[0] = 2;
  EXPECT_THROW(DecodeRequest(version), CodecError);
  auto kind = frame;
  kind[1] = 3;
  EXPECT_THROW(DecodeRequest(kind), CodecError);
  auto operation = frame;
  operation[26] = 9;
  EXPECT_THROW(DecodeRequest(operation), CodecError);
  EXPECT_THROW(DecodeRequest(Request(kAlice, 0, Get{.key = "k"})), CodecError);
  auto boolean = EncodeResponse(Value{.found = true, .value = ""});
  boolean[3] = 2;
  EXPECT_THROW(DecodeResponse(boolean), CodecError);
  EXPECT_THROW(DecodeResponse(EncodeRead("k")), CodecError);
  auto missing_with_bytes = EncodeResponse(Value{.found = true, .value = "x"});
  missing_with_bytes[3] = 0;
  EXPECT_THROW(DecodeResponse(missing_with_bytes), CodecError);
  EXPECT_THROW(
      EncodeRequest({.client = kAlice,
                     .sequence = 1,
                     .operation = Put{.key = "k", .value = std::string(kMaxBlobBytes, 'x')}}),
      CodecError);
}

TEST(KvStore, OperationsFollowDocumentedSemantics) {
  Store store;
  EXPECT_EQ(Apply(store, kAlice, 1, Get{.key = "k"}), (Response{Value{}}));
  EXPECT_EQ(Apply(store, kAlice, 2, Cas{.key = "k", .expected = "", .replacement = "x"}),
            (Response{CasResult{}}));
  EXPECT_EQ(ReadKey(store, "k"), (Response{Value{}}));
  EXPECT_EQ(Apply(store, kAlice, 3, Append{.key = "k", .suffix = "a"}), (Response{WriteOk{}}));
  EXPECT_EQ(Apply(store, kAlice, 4, Append{.key = "k", .suffix = "b"}), (Response{WriteOk{}}));
  EXPECT_EQ(ReadKey(store, "k"), (Response{Value{.found = true, .value = "ab"}}));
  EXPECT_EQ(Apply(store, kAlice, 5, Cas{.key = "k", .expected = "a", .replacement = "c"}),
            (Response{CasResult{.exchanged = false, .found = true, .previous = "ab"}}));
  EXPECT_EQ(Apply(store, kAlice, 6, Cas{.key = "k", .expected = "ab", .replacement = ""}),
            (Response{CasResult{.exchanged = true, .found = true, .previous = "ab"}}));
  EXPECT_EQ(ReadKey(store, "k"), (Response{Value{.found = true, .value = ""}}));
  EXPECT_EQ(Apply(store, kAlice, 7, Put{.key = "k", .value = "z"}), (Response{WriteOk{}}));
  EXPECT_EQ(Apply(store, kAlice, 8, Get{.key = "k"}),
            (Response{Value{.found = true, .value = "z"}}));
}

TEST(KvStore, RetriedSequenceAppliesOnceAndReturnsTheOriginalResult) {
  Store store;
  EXPECT_EQ(Apply(store, kAlice, 1, Append{.key = "k", .suffix = "a"}), (Response{WriteOk{}}));
  EXPECT_EQ(Apply(store, kAlice, 1, Append{.key = "k", .suffix = "a"}), (Response{WriteOk{}}));
  EXPECT_EQ(Apply(store, kAlice, 2, Get{.key = "k"}),
            (Response{Value{.found = true, .value = "a"}}));
  EXPECT_EQ(Apply(store, kBob, 1, Append{.key = "k", .suffix = "b"}), (Response{WriteOk{}}));
  // The cached Get result is returned even though the key has changed since.
  EXPECT_EQ(Apply(store, kAlice, 2, Get{.key = "k"}),
            (Response{Value{.found = true, .value = "a"}}));
  EXPECT_EQ(Apply(store, kAlice, 1, Append{.key = "k", .suffix = "a"}),
            (Response{StaleSequence{}}));
  // Sequence gaps are accepted: a client may abandon a request it never saw answered.
  EXPECT_EQ(Apply(store, kAlice, 9, Append{.key = "k", .suffix = "c"}), (Response{WriteOk{}}));
  EXPECT_EQ(ReadKey(store, "k"), (Response{Value{.found = true, .value = "abc"}}));
}

TEST(KvStore, MalformedCommandsHaveNoEffectButAreStillDeterministic) {
  Store one;
  Store two;
  const Bytes garbage{1, 1, 7};
  EXPECT_EQ(DecodeResponse(one.Apply(garbage)), (Response{Invalid{}}));
  static_cast<void>(two.Apply(garbage));
  EXPECT_TRUE(one.data().empty());
  EXPECT_EQ(one.digest(), two.digest());
}

TEST(KvStore, SnapshotRestoresDataSessionsAndDigestExactly) {
  Store store;
  static_cast<void>(Apply(store, kAlice, 1, Put{.key = "a", .value = std::string("\0x", 2)}));
  static_cast<void>(Apply(store, kBob, 4, Append{.key = "b", .suffix = "y"}));
  Store copy;
  copy.Restore(store.Snapshot());
  EXPECT_EQ(copy.data(), store.data());
  EXPECT_EQ(copy.digest(), store.digest());
  EXPECT_EQ(copy.Snapshot(), store.Snapshot());
  // Session dedupe survives the snapshot.
  EXPECT_EQ(Apply(copy, kBob, 4, Append{.key = "b", .suffix = "y"}), (Response{WriteOk{}}));
  EXPECT_EQ(ReadKey(copy, "b"), (Response{Value{.found = true, .value = "y"}}));
  EXPECT_EQ(Apply(copy, kBob, 3, Append{.key = "b", .suffix = "y"}), (Response{StaleSequence{}}));
}

TEST(KvStore, InvalidSnapshotLeavesStateUntouched) {
  Store store;
  static_cast<void>(Apply(store, kAlice, 1, Put{.key = "a", .value = "1"}));
  const auto before = store.Snapshot();
  auto snapshot = before;
  snapshot.pop_back();
  EXPECT_THROW(store.Restore(snapshot), CodecError);
  EXPECT_THROW(store.Restore(EncodeRead("a")), CodecError);
  EXPECT_EQ(store.Snapshot(), before);
}

TEST(KvStore, ReplicasApplyingTheSameCommandsAgree) {
  Store one;
  Store two;
  for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
    const auto command =
        Request(sequence % 2 == 0 ? kAlice : kBob, (sequence + 1) / 2,
                Append{.key = std::to_string(sequence % 3), .suffix = std::to_string(sequence)});
    EXPECT_EQ(one.Apply(command), two.Apply(command));
  }
  EXPECT_EQ(one.data(), two.data());
  EXPECT_EQ(one.digest(), two.digest());
}

}  // namespace
}  // namespace quorumkv::kv
