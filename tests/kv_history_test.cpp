#include "quorumkv/kv_history.hpp"

#include <gtest/gtest.h>

namespace quorumkv::sim {
namespace {

auto Append(std::uint64_t client, const std::string& token, std::uint64_t invoke,
            std::optional<std::uint64_t> complete) -> HistoryOp {
  return {.client = client,
          .kind = OpKind::Append,
          .key = "a",
          .argument = token,
          .expected = {},
          .invoke = invoke,
          .complete = complete,
          .found = false,
          .value = {},
          .exchanged = false};
}

auto Get(const std::string& key, std::uint64_t invoke, std::uint64_t complete,
         const std::optional<std::string>& value) -> HistoryOp {
  return {.client = 9,
          .kind = OpKind::Get,
          .key = key,
          .argument = {},
          .expected = {},
          .invoke = invoke,
          .complete = complete,
          .found = value.has_value(),
          .value = value.value_or(""),
          .exchanged = false};
}

auto Put(const std::string& value, std::uint64_t invoke,
         std::optional<std::uint64_t> complete) -> HistoryOp {
  return {.client = 1,
          .kind = OpKind::Put,
          .key = "r",
          .argument = value,
          .expected = {},
          .invoke = invoke,
          .complete = complete,
          .found = false,
          .value = {},
          .exchanged = false};
}

void ExpectViolation(const std::vector<HistoryOp>& history,
                     const std::map<std::string, std::string>& final_values,
                     const std::string& property) {
  try {
    CheckHistory(history, final_values);
    ADD_FAILURE() << "expected " << property;
  } catch (const std::logic_error& error) {
    EXPECT_EQ(std::string(error.what()).rfind(property, 0), 0U) << error.what();
  }
}

TEST(KvHistory, AcceptsConcurrentAndUnknownOutcomes) {
  const std::vector<HistoryOp> history{
      Append(1, "x;", 1, 4),
      Append(2, "y;", 2, std::nullopt),  // Timed out but applied.
      Append(3, "z;", 3, std::nullopt),  // Timed out and never applied.
      Get("a", 5, 6, "x;"),              // y may be ordered after this read.
      Get("a", 7, 8, "x;y;"),
      Put("p", 9, 10),
      Get("r", 2, 3, std::nullopt),
      Get("r", 11, 12, "p"),
  };
  EXPECT_NO_THROW(CheckHistory(history, {{"a", "x;y;"}}));
}

TEST(KvHistory, FlagsDuplicateApplication) {
  ExpectViolation({Append(1, "x;", 1, 2)}, {{"a", "x;x;"}}, "Exactly-once");
}

TEST(KvHistory, FlagsLostAcknowledgedWrite) {
  ExpectViolation({Append(1, "x;", 1, 2), Append(1, "y;", 3, 4)}, {{"a", "x;"}}, "Durability");
}

TEST(KvHistory, FlagsStaleRead) {
  ExpectViolation({Append(1, "x;", 1, 2), Append(1, "y;", 3, 4), Get("a", 5, 6, "x;")},
                  {{"a", "x;y;"}}, "Stale read");
}

TEST(KvHistory, FlagsReadOfStateThatNeverCommitted) {
  ExpectViolation({Append(1, "x;", 1, std::nullopt), Append(2, "y;", 2, 5), Get("a", 3, 4, "x;")},
                  {{"a", "y;"}}, "Read of a state that never committed");
}

TEST(KvHistory, FlagsReadFromTheFuture) {
  ExpectViolation({Get("a", 1, 2, "x;"), Append(1, "x;", 3, 4)}, {{"a", "x;"}},
                  "Read from the future");
}

TEST(KvHistory, FlagsRealTimeOrderInversion) {
  ExpectViolation({Append(1, "x;", 1, 2), Append(2, "y;", 3, 4)}, {{"a", "y;x;"}},
                  "Real-time order");
}

TEST(KvHistory, FlagsFabricatedAndMalformedValues) {
  ExpectViolation({Append(1, "x;", 1, 2)}, {{"a", "x;w;"}}, "Fabricated write");
  ExpectViolation({Append(1, "x;", 1, 2)}, {{"a", "x;w"}}, "Malformed append value");
}

TEST(KvHistory, FlagsStaleAndUnexplainedRegisterReads) {
  ExpectViolation({Put("p", 1, 2), Get("r", 3, 4, std::nullopt)}, {}, "Stale read");
  ExpectViolation({Put("p", 1, 2), Get("r", 3, 4, "q")}, {}, "Unexplained read");
  ExpectViolation({Get("r", 1, 2, "p"), Put("p", 3, 4)}, {}, "Unexplained read");
}

TEST(KvHistory, RejectsInconsistentWorkloads) {
  ExpectViolation({Append(1, "x;", 1, 2), Append(2, "x;", 3, 4)}, {{"a", "x;"}}, "Workload error");
  ExpectViolation({Append(1, "x;", 3, 2)}, {{"a", "x;"}}, "Workload error");
}

}  // namespace
}  // namespace quorumkv::sim
