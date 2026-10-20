#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <string>

#include "quorumkv/codec.hpp"
#include "reference_codec.hpp"
#include "seed_config.hpp"

namespace quorumkv::test {
namespace {

class SeededRandom {
 public:
  explicit SeededRandom(std::uint64_t seed) : state_(seed) {}

  auto Next() -> std::uint64_t {
    // SplitMix64: specified arithmetic, independent of standard-library distributions.
    auto value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  auto Blob() -> Bytes {
    Bytes bytes(static_cast<std::size_t>(Next() % 257));
    for (auto& byte : bytes) {
      byte = static_cast<std::uint8_t>(Next());
    }
    return bytes;
  }

 private:
  std::uint64_t state_;
};

auto Generate(SeededRandom& random, std::size_t kind) -> Message {
  Message message{.from = random.Next(),
                  .to = random.Next(),
                  .term = random.Next(),
                  .rpc_id = random.Next(),
                  .body = RequestVote{}};
  switch (kind) {
    case 0:
      message.body = RequestVote{.last_log_index = random.Next(), .last_log_term = random.Next()};
      break;
    case 1:
      message.body = VoteResponse{random.Next() % 2 == 1};
      break;
    case 2: {
      AppendEntries body{.prev_log_index = random.Next(),
                         .prev_log_term = random.Next(),
                         .leader_commit = random.Next(),
                         .read_context = random.Next(),
                         .entries = {}};
      const auto count = random.Next() % 9;
      for (std::uint64_t i = 0; i < count; ++i) {
        LogEntry entry{.index = random.Next(),
                       .term = random.Next(),
                       .kind = static_cast<EntryKind>(random.Next() % 2),
                       .command = {}};
        if (entry.kind == EntryKind::Command) {
          entry.command = random.Blob();
        }
        body.entries.push_back(std::move(entry));
      }
      message.body = std::move(body);
      break;
    }
    case 3: {
      AppendResponse body{.success = random.Next() % 2 == 1,
                          .match_index = random.Next(),
                          .conflict_term = std::nullopt,
                          .conflict_index = random.Next(),
                          .read_context = random.Next()};
      if (random.Next() % 2 == 1) {
        body.conflict_term = random.Next();
      }
      message.body = body;
      break;
    }
    case 4:
      message.body = InstallSnapshot{.last_included_index = random.Next(),
                                     .last_included_term = random.Next(),
                                     .offset = random.Next(),
                                     .done = random.Next() % 2 == 1,
                                     .data = random.Blob()};
      break;
    case 5:
      message.body =
          SnapshotResponse{.success = random.Next() % 2 == 1, .next_offset = random.Next()};
      break;
    default:
      throw std::invalid_argument("unknown generator kind");
  }
  return message;
}

void Hash(std::uint64_t& digest, const Bytes& bytes) {
  for (auto byte : bytes) {
    digest = (digest ^ byte) * 0x100000001b3ULL;
  }
}

struct ProbeCounts {
  std::uint64_t accepted{};
  std::uint64_t rejected{};
};

void CheckArbitraryFrame(const Bytes& frame, ProbeCounts& counts) {
  Message decoded;
  try {
    decoded = Decode(frame);
  } catch (const CodecError&) {
    ++counts.rejected;
    return;
  }
  // Accepted bytes must be canonical, including boolean/optional encodings.
  EXPECT_EQ(Encode(decoded), frame);
  EXPECT_EQ(ReferenceEncode(decoded), frame);
  ++counts.accepted;
}

TEST(SeedOptions, DefaultsAndSingleSeedReplay) {
  const auto defaults = ParseSeedOptions({});
  EXPECT_EQ(defaults.first, 0U);
  EXPECT_EQ(defaults.count, 10000U);
  const auto replay =
      ParseSeedOptions(std::array<std::string_view, 1>{"--seed=18446744073709551615"});
  EXPECT_EQ(replay.first, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(replay.count, 1U);
  const auto range =
      ParseSeedOptions(std::array<std::string_view, 2>{"--seed-count=20", "--seed=10"});
  EXPECT_EQ(range.first, 10U);
  EXPECT_EQ(range.count, 20U);
}

TEST(SeedOptions, RejectsInvalidAndWrappingRanges) {
  for (const auto option :
       {"--seed=", "--seed=-1", "--seed=1x", "--seed=18446744073709551616", "--seed-count=0",
        "--seed-count=-2", "--seed=+1", "--seed", "--seeds=2"}) {
    SCOPED_TRACE(option);
    const std::array<std::string_view, 1> options{option};
    EXPECT_THROW(ParseSeedOptions(options), std::invalid_argument);
  }
  const std::array<std::string_view, 2> duplicate{"--seed=1", "--seed=2"};
  EXPECT_THROW(ParseSeedOptions(duplicate), std::invalid_argument);
  const std::array<std::string_view, 2> duplicate_count{"--seed-count=1", "--seed-count=2"};
  EXPECT_THROW(ParseSeedOptions(duplicate_count), std::invalid_argument);
  const std::array<std::string_view, 2> wrap{"--seed=18446744073709551615", "--seed-count=2"};
  EXPECT_THROW(ParseSeedOptions(wrap), std::invalid_argument);
}

TEST(SeedOptions, GeneratorHasStableKnownSequence) {
  SeededRandom random(0);
  EXPECT_EQ(random.Next(), 0xe220a8397b1dcdafULL);
  EXPECT_EQ(random.Next(), 0x6e789e6aa1b965f4ULL);
}

TEST(CodecFuzz, RoundTripsMatchReferenceAndMalformedFramesAreSafe) {
  std::uint64_t digest = 0xcbf29ce484222325ULL;
  std::uint64_t round_trips = 0;
  ProbeCounts counts;
  for (std::uint64_t offset = 0; offset < seed_config.count; ++offset) {
    const auto seed = seed_config.first + offset;
    SCOPED_TRACE("replay: quorumkv_tests --seed=" + std::to_string(seed) +
                 " --gtest_filter=CodecFuzz.*");
    SeededRandom random(seed);
    try {
      for (std::size_t kind = 0; kind < 6; ++kind) {
        SCOPED_TRACE("message kind=" + std::to_string(kind));
        const auto message = Generate(random, kind);
        const auto frame = Encode(message);
        ASSERT_EQ(frame, ReferenceEncode(message));
        ASSERT_EQ(Decode(frame), message);
        ASSERT_EQ(Encode(Decode(frame)), frame);
        Hash(digest, frame);
        ++round_trips;

        auto corrupt = frame;
        const auto position = static_cast<std::size_t>(random.Next() % corrupt.size());
        corrupt[position] ^= static_cast<std::uint8_t>(1 + random.Next() % 255);
        CheckArbitraryFrame(corrupt, counts);
        Hash(digest, corrupt);
        ASSERT_FALSE(::testing::Test::HasFailure());
      }
      auto arbitrary = random.Blob();
      CheckArbitraryFrame(arbitrary, counts);
      // Repair its outer length to also drive the decoder beyond the prefix.
      if (arbitrary.size() >= 4) {
        Bytes length;
        ReferenceInteger<4>(length, arbitrary.size() - 4);
        std::ranges::copy(length, arbitrary.begin());
      }
      CheckArbitraryFrame(arbitrary, counts);
      Hash(digest, arbitrary);
      ASSERT_FALSE(::testing::Test::HasFailure());
    } catch (const std::exception& error) {
      FAIL() << "unexpected exception: " << error.what();
    }
  }
  std::cout << "[fuzz] first_seed=" << seed_config.first << " seeds=" << seed_config.count
            << " round_trips=" << round_trips << " probe_accepted=" << counts.accepted
            << " probe_rejected=" << counts.rejected << " corpus_digest=" << std::hex << digest
            << std::dec << '\n';
}

}  // namespace
}  // namespace quorumkv::test
