#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "quorumkv/codec.hpp"
#include "quorumkv/kv.hpp"
#include "quorumkv/kv_workload.hpp"
#include "seed_config.hpp"

namespace quorumkv::sim {
namespace {

// Test-only broken store: forgets sessions, so every retry executes again.
class NoDedupeStore final : public StateMachine {
 public:
  auto Apply(std::span<const std::uint8_t> command) -> Bytes override {
    try {
      auto request = kv::DecodeRequest(command);
      request.sequence = ++fresh_;
      return store_.Apply(kv::EncodeRequest(request));
    } catch (const CodecError&) {
      return store_.Apply(command);
    }
  }
  [[nodiscard]] auto Read(std::span<const std::uint8_t> query) const -> Bytes override {
    return store_.Read(query);
  }
  [[nodiscard]] auto Snapshot() const -> Bytes override { return store_.Snapshot(); }
  void Restore(std::span<const std::uint8_t> snapshot) override { store_.Restore(snapshot); }

 private:
  kv::Store store_;
  std::uint64_t fresh_{};
};

// Searches seeds for the first checker violation of a deliberately broken mode.
auto FirstViolation(const KvScenarioConfig& base,
                    std::uint64_t limit) -> std::pair<std::uint64_t, std::string> {
  for (std::uint64_t seed = 0; seed < limit; ++seed) {
    auto config = base;
    config.seed = seed;
    try {
      static_cast<void>(RunKvScenario(config));
    } catch (const std::logic_error& error) {
      return {seed, error.what()};
    }
  }
  return {limit, ""};
}

TEST(KvFuzz, ExactlyOnceAndFreshReadsAcrossSeededFaults) {
  const auto config = test::seed_config;
  KvScenarioResult totals;
  std::uint64_t operations = 0;
  std::uint64_t acknowledged = 0;
  std::uint64_t digest = 14695981039346656037ULL;
  for (std::uint64_t offset = 0; offset < config.count; ++offset) {
    const auto seed = config.first + offset;
    const auto replay =
        "quorumkv_tests --gtest_filter=KvFuzz.Exactly* --seed=" + std::to_string(seed);
    try {
      const auto result = RunKvScenario({.seed = seed, .record_trace = config.count == 1});
      if (config.count == 1) {
        std::cout << "[sim-trace-begin]\n" << result.trace << "[sim-trace-end]\n";
      }
      operations += result.history.size();
      acknowledged += static_cast<std::uint64_t>(std::ranges::count_if(
          result.history, [](const auto& op) { return op.complete.has_value(); }));
      totals.retries += result.retries;
      totals.timeouts += result.timeouts;
      totals.not_leader += result.not_leader;
      totals.logged_reads += result.logged_reads;
      totals.quiesce_ticks = std::max(totals.quiesce_ticks, result.quiesce_ticks);
      totals.stats.steps += result.stats.steps;
      totals.stats.safety_checks += result.stats.safety_checks;
      totals.stats.applied_commands += result.stats.applied_commands;
      totals.stats.reads_served += result.stats.reads_served;
      totals.stats.reads_failed += result.stats.reads_failed;
      totals.stats.crashes += result.stats.crashes;
      totals.stats.partitions += result.stats.partitions;
      digest = (digest ^ result.digest) * 1099511628211ULL;
    } catch (const std::exception& error) {
      std::cerr << "[sim-failure] seed=" << seed << " replay=" << replay << '\n';
      if (config.count != 1) {
        try {
          static_cast<void>(RunKvScenario({.seed = seed, .record_trace = true}));
        } catch (const std::exception& replay_error) {
          EXPECT_STREQ(error.what(), replay_error.what());
        }
      }
      FAIL() << error.what() << "; seed=" << seed << "; replay: " << replay;
    }
  }
  std::cout << "[kv] first_seed=" << config.first << " seeds=" << config.count
            << " operations=" << operations << " acknowledged=" << acknowledged
            << " retries=" << totals.retries << " timeouts=" << totals.timeouts
            << " not_leader=" << totals.not_leader << " logged_reads=" << totals.logged_reads
            << " reads_served=" << totals.stats.reads_served
            << " reads_failed=" << totals.stats.reads_failed
            << " applied_commands=" << totals.stats.applied_commands
            << " crashes=" << totals.stats.crashes << " partitions=" << totals.stats.partitions
            << " steps=" << totals.stats.steps << " safety_checks=" << totals.stats.safety_checks
            << " max_quiesce_ticks=" << totals.quiesce_ticks << " digest=" << std::hex << digest
            << std::dec << '\n';
}

TEST(KvFuzz, SeedReplaysIdentically) {
  const auto first = RunKvScenario({.seed = 42, .record_trace = true});
  const auto second = RunKvScenario({.seed = 42, .record_trace = true});
  EXPECT_FALSE(first.trace.empty());
  EXPECT_EQ(first.trace, second.trace);
  EXPECT_EQ(first.digest, second.digest);
  EXPECT_EQ(first.final_values, second.final_values);
  EXPECT_GT(first.retries, 0U);
}

TEST(KvFuzz, CheckerFlagsUnsafeLocalReads) {
  const auto [seed, error] = FirstViolation({.unsafe_reads = true}, 1000);
  std::cout << "[kv-negative] mode=unsafe_reads first_seed=" << seed << " error=" << error << '\n';
  ASSERT_LT(seed, 1000U) << "the checker never flagged reads served from local state";
  EXPECT_TRUE(error.starts_with("Stale read") ||
              error.starts_with("Read of a state that never committed"))
      << error;
}

TEST(KvFuzz, CheckerFlagsMissingSessionDedupe) {
  const auto [seed, error] =
      FirstViolation({.machine_factory = [] { return std::make_unique<NoDedupeStore>(); }}, 1000);
  std::cout << "[kv-negative] mode=no_dedupe first_seed=" << seed << " error=" << error << '\n';
  ASSERT_LT(seed, 1000U) << "the checker never flagged duplicate application of a retry";
  EXPECT_TRUE(error.starts_with("Exactly-once")) << error;
}

}  // namespace
}  // namespace quorumkv::sim
