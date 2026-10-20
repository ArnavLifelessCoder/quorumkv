#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

#include "quorumkv/sim_cluster.hpp"
#include "seed_config.hpp"

namespace quorumkv::sim {
namespace {

constexpr unsigned kElectionBound = 300;

struct ElectionResult {
  NodeId leader{};
  unsigned ticks{};
};

auto AwaitLeader(Cluster& cluster) -> ElectionResult {
  for (unsigned tick = 1; tick <= kElectionBound; ++tick) {
    cluster.Advance();
    if (const auto leaders = cluster.Leaders(); leaders.size() == 1) {
      return {.leader = leaders.front(), .ticks = tick};
    }
  }
  throw std::runtime_error("leader election exceeded 300 ticks");
}

struct ScenarioResult {
  unsigned cold_ticks{};
  unsigned reelection_ticks{};
  unsigned recovery_ticks{};
  SimStats stats;
  std::uint64_t digest{};
};

auto Scenario(std::uint64_t seed, bool trace) -> ScenarioResult {
  const std::size_t size = seed % 2 == 0 ? 3 : 5;
  Cluster cluster({.seed = seed, .size = size, .record_trace = trace});
  SeededRandom nemesis(seed ^ 0x72616674656c6563ULL);
  try {
    cluster.SetFaults({.drop_percent = 5 + nemesis.Next() % 6,
                       .duplicate_percent = nemesis.Next() % 21,
                       .max_delay = 1 + nemesis.Next() % 3});
    const auto cold = AwaitLeader(cluster);
    const auto old_term = cluster.State(cold.leader).term;
    cluster.Crash(cold.leader);
    const auto replacement = AwaitLeader(cluster);
    if (replacement.leader == cold.leader || cluster.State(replacement.leader).term <= old_term) {
      throw std::logic_error("crashed leader was not replaced in a higher term");
    }
    cluster.Restart(cold.leader);
    cluster.SetVoteCrashPercent(20 + nemesis.Next() % 41);
    // Six partition/heal cycles, with packet loss and duplication still enabled.
    for (unsigned tick = 0; tick < 180; ++tick) {
      if (tick % 30 == 0) {
        // Alternate asynchronous safety phases and short-delay crash phases.
        // Only the cold-start/re-election and healed phases have liveness bounds.
        cluster.SetFaults(
            {.drop_percent = nemesis.Next() % 21,
             .duplicate_percent = nemesis.Next() % 41,
             .max_delay = tick % 60 == 0 ? 31 + nemesis.Next() % 30 : 1 + nemesis.Next() % 3});
        std::vector<std::uint64_t> groups(size);
        for (auto& group : groups) {
          group = nemesis.Next() % 2;
        }
        groups.front() = 0;
        groups.back() = 1;
        cluster.Partition(groups);
      }
      if (tick % 30 == 15) {
        cluster.Heal();
      }
      if (tick % 45 == 10) {
        cluster.Crash(1 + nemesis.Next() % size);
      }
      if (tick % 5 == 4) {
        for (NodeId id = 1; id <= size; ++id) {
          cluster.Restart(id);
        }
      }
      if (tick % 7 == 0) {
        cluster.FailNextSync(1 + nemesis.Next() % size, nemesis.Next() % 2 == 0
                                                            ? SyncFailure::BeforeSync
                                                            : SyncFailure::AfterSync);
      }
      cluster.Advance();
    }
    cluster.Heal();
    cluster.SetVoteCrashPercent(0);
    cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
    for (NodeId id = 1; id <= size; ++id) {
      cluster.FailNextSync(id, SyncFailure::None);
      cluster.Restart(id);
    }
    // Drain delayed packets and give formerly isolated leaders time to step down.
    for (unsigned tick = 0; tick < 90; ++tick) {
      cluster.Advance();
    }
    const auto recovered = AwaitLeader(cluster);
    const auto stats = cluster.Stats();
    if (stats.steps != stats.safety_checks || stats.dropped == 0 || stats.partitions != 6) {
      throw std::logic_error("scenario did not exercise the required fault/safety checks");
    }
    if (trace) {
      std::cout << "[sim-trace-begin]\n" << cluster.Trace() << "[sim-trace-end]\n";
    }
    return {.cold_ticks = cold.ticks,
            .reelection_ticks = replacement.ticks,
            .recovery_ticks = 90 + recovered.ticks,
            .stats = stats,
            .digest = cluster.Digest()};
  } catch (const std::exception&) {
    if (trace) {
      std::cout << "[sim-trace-begin]\n" << cluster.Trace() << "[sim-trace-end]\n";
    }
    throw;
  }
}

TEST(ElectionFuzz, SafetyAndRecoveryAcrossSeededFaultSchedules) {
  const auto config = test::seed_config;
  ScenarioResult totals;
  std::vector<unsigned> cold_ticks;
  std::vector<unsigned> reelection_ticks;
  std::uint64_t digest = 14695981039346656037ULL;
  for (std::uint64_t offset = 0; offset < config.count; ++offset) {
    const auto seed = config.first + offset;
    const auto replay =
        "quorumkv_tests --gtest_filter=ElectionFuzz.* --seed=" + std::to_string(seed);
    SCOPED_TRACE(replay);
    try {
      const auto result = Scenario(seed, config.count == 1);
      cold_ticks.push_back(result.cold_ticks);
      reelection_ticks.push_back(result.reelection_ticks);
      totals.cold_ticks = std::max(totals.cold_ticks, result.cold_ticks);
      totals.reelection_ticks = std::max(totals.reelection_ticks, result.reelection_ticks);
      totals.recovery_ticks = std::max(totals.recovery_ticks, result.recovery_ticks);
      totals.stats.steps += result.stats.steps;
      totals.stats.safety_checks += result.stats.safety_checks;
      totals.stats.delivered += result.stats.delivered;
      totals.stats.dropped += result.stats.dropped;
      totals.stats.duplicated += result.stats.duplicated;
      totals.stats.partitions += result.stats.partitions;
      totals.stats.crashes += result.stats.crashes;
      totals.stats.restarts += result.stats.restarts;
      totals.stats.sync_failures += result.stats.sync_failures;
      totals.stats.vote_grant_crashes += result.stats.vote_grant_crashes;
      totals.stats.long_delay_packets += result.stats.long_delay_packets;
      totals.stats.stale_vote_deliveries += result.stats.stale_vote_deliveries;
      digest = (digest ^ result.digest) * 1099511628211ULL;
    } catch (const std::exception& error) {
      std::cerr << "[sim-failure] seed=" << seed << " replay=" << replay << '\n';
      if (config.count != 1) {
        try {
          Scenario(seed, true);
        } catch (const std::exception& replay_error) {
          EXPECT_STREQ(error.what(), replay_error.what());
        }
      }
      FAIL() << error.what() << "; seed=" << seed << "; replay: " << replay;
    }
  }
  auto percentile = [](std::vector<unsigned>& values, std::size_t percent) {
    std::ranges::sort(values);
    return values.at((values.size() * percent + 99) / 100 - 1);
  };
  std::cout << "[sim] first_seed=" << config.first << " seeds=" << config.count
            << " p50_cold_ticks=" << percentile(cold_ticks, 50)
            << " p99_cold_ticks=" << percentile(cold_ticks, 99)
            << " p50_reelection_ticks=" << percentile(reelection_ticks, 50)
            << " p99_reelection_ticks=" << percentile(reelection_ticks, 99)
            << " election_bound=" << kElectionBound << " max_cold_ticks=" << totals.cold_ticks
            << " max_reelection_ticks=" << totals.reelection_ticks
            << " max_recovery_ticks=" << totals.recovery_ticks << " steps=" << totals.stats.steps
            << " safety_checks=" << totals.stats.safety_checks
            << " delivered=" << totals.stats.delivered << " dropped=" << totals.stats.dropped
            << " duplicated=" << totals.stats.duplicated
            << " partitions=" << totals.stats.partitions << " crashes=" << totals.stats.crashes
            << " restarts=" << totals.stats.restarts
            << " sync_failures=" << totals.stats.sync_failures
            << " vote_grant_crashes=" << totals.stats.vote_grant_crashes
            << " long_delay_packets=" << totals.stats.long_delay_packets
            << " stale_vote_deliveries=" << totals.stats.stale_vote_deliveries
            << " digest=" << std::hex << digest << std::dec << '\n';
}

}  // namespace
}  // namespace quorumkv::sim
