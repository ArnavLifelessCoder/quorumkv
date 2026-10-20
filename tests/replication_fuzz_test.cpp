#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "figure8_scenario.hpp"
#include "persistence_scenario.hpp"
#include "quorumkv/sim_cluster.hpp"
#include "seed_config.hpp"

namespace quorumkv::sim {
namespace {

struct Result {
  SimStats stats;
  std::uint64_t commands{};
  unsigned recovery{};
  std::uint64_t digest{};
  std::uint64_t figure8_runs{};
  std::uint64_t ack_crash_runs{};
};

auto ReplicationScenario(std::uint64_t seed, bool trace) -> Result {
  const std::size_t size = seed % 2 == 0 ? 3 : 5;
  Cluster cluster(
      {.seed = seed, .size = size, .record_trace = trace, .max_append_entries = 1 + seed % 8});
  SeededRandom nemesis(seed ^ 0x6c6f677361666574ULL);
  std::unique_ptr<Cluster> directed;
  std::string_view phase;
  const auto print_trace = [&] {
    if (trace) {
      std::cout << "[sim-trace-begin]\n";
      if (directed) {
        std::cout << phase << '\n' << directed->Trace() << "random-faults\n";
      }
      std::cout << cluster.Trace() << "[sim-trace-end]\n";
    }
  };
  try {
    if (seed % 8 == 7) {
      phase = "figure8";
      auto config = figure8::Figure8Config(seed);
      config.record_trace = trace;
      directed = std::make_unique<Cluster>(std::move(config));
      figure8::ElectS1AndReplicateOldTermToMajority(*directed);
      // Let a term-3 log win the next election. An incorrect old-term commit
      // is exposed by the historical Leader Completeness oracle.
      figure8::S5Campaign(*directed);
      for (unsigned step = 0; step < 100 && !directed->Pending().empty(); ++step) {
        directed->Deliver(directed->Pending().front().first);
      }
      if (directed->CommitIndex(5) < 3) {
        throw std::runtime_error("Figure 8 replacement did not commit");
      }
    } else if (seed % 8 == 6) {
      phase = "ack-crash";
      directed = std::make_unique<Cluster>(
          ClusterConfig{.seed = seed, .record_trace = trace, .manual_delivery = true});
      CrashAfterAcknowledgement(*directed);
    }
    cluster.SetFaults({.drop_percent = nemesis.Next() % 11,
                       .duplicate_percent = nemesis.Next() % 31,
                       .max_delay = 3});
    for (unsigned tick = 0; tick < 300 && cluster.Leaders().empty(); ++tick) {
      cluster.Advance();
    }
    if (cluster.Leaders().size() != 1) {
      throw std::runtime_error("replication cold start exceeded 300 ticks");
    }
    const auto leader = cluster.Leaders().front();
    const auto first = cluster.Propose(leader, {0});
    for (unsigned tick = 0; tick < 300 && cluster.Committed().size() < first.value(); ++tick) {
      cluster.Advance();
    }
    if (cluster.Committed().size() < first.value()) {
      throw std::runtime_error("initial proposal exceeded 300 ticks");
    }
    cluster.SetVoteCrashPercent(20 + nemesis.Next() % 31);
    cluster.SetReplicationCrashPercent(5 + nemesis.Next() % 11, 5 + nemesis.Next() % 16);
    for (unsigned tick = 0; tick < 240; ++tick) {
      if (tick % 40 == 0) {
        cluster.SetFaults(
            {.drop_percent = nemesis.Next() % 26,
             .duplicate_percent = nemesis.Next() % 41,
             .max_delay = tick % 80 == 0 ? 31 + nemesis.Next() % 40 : 1 + nemesis.Next() % 3});
        std::vector<std::uint64_t> groups(size);
        for (auto& group : groups) {
          group = nemesis.Next() % 2;
        }
        groups.front() = 0;
        groups.back() = 1;
        cluster.Partition(groups);
      }
      if (tick % 40 == 20) {
        cluster.Heal();
      }
      if (tick % 11 == 0) {
        cluster.FailNextSync(1 + nemesis.Next() % size, nemesis.Next() % 2 == 0
                                                            ? SyncFailure::BeforeSync
                                                            : SyncFailure::AfterSync);
      }
      if (tick % 29 == 0) {
        const auto leaders = cluster.Leaders();
        cluster.Crash(leaders.empty() ? 1 + nemesis.Next() % size : leaders.front());
      }
      if (tick % 7 == 6) {
        for (NodeId id = 1; id <= size; ++id) {
          cluster.Restart(id);
        }
      }
      if (tick % 4 == 0) {
        for (const auto id : cluster.Leaders()) {
          Bytes command;
          for (const auto value : {seed, static_cast<std::uint64_t>(tick), id}) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
              command.push_back(static_cast<std::uint8_t>(value >> shift));
            }
          }
          static_cast<void>(cluster.Propose(id, std::move(command)));
        }
      }
      cluster.Advance();
    }
    cluster.Heal();
    cluster.SetVoteCrashPercent(0);
    cluster.SetReplicationCrashPercent(0, 0);
    cluster.SetFaults({.drop_percent = 0, .duplicate_percent = 0, .max_delay = 1});
    // A simultaneous crash forces every node to recover only from durable state.
    for (NodeId id = 1; id <= size; ++id) {
      cluster.Crash(id);
      cluster.FailNextSync(id, SyncFailure::None);
    }
    for (NodeId id = 1; id <= size; ++id) {
      cluster.Restart(id);
    }
    unsigned recovery = 0;
    bool converged = false;
    for (; recovery < 600; ++recovery) {
      cluster.Advance();
      // Drain the maximum delayed packet horizon before declaring convergence.
      if (recovery < 70 || cluster.Leaders().size() != 1) {
        continue;
      }
      const auto expected = cluster.Log(cluster.Leaders().front()).entries;
      converged = true;
      for (NodeId id = 1; id <= size; ++id) {
        converged = converged && cluster.Log(id).entries == expected &&
                    cluster.CommitIndex(id) == expected.size();
      }
      if (converged) {
        break;
      }
    }
    if (!converged) {
      throw std::runtime_error("healed replication did not converge within 600 ticks");
    }
    auto stats = cluster.Stats();
    if (stats.steps != stats.safety_checks || stats.log_checks != size * stats.steps ||
        stats.application_checks != size * stats.steps || stats.partitions != 6) {
      throw std::logic_error("replication safety checks skipped a simulated step");
    }
    auto commands = static_cast<std::uint64_t>(std::ranges::count_if(
        cluster.Committed(),
        [](const LogEntry& entry) { return entry.kind == EntryKind::Command; }));
    auto digest = cluster.Digest();
    if (directed) {
      const auto extra = directed->Stats();
      stats.steps += extra.steps;
      stats.safety_checks += extra.safety_checks;
      stats.log_checks += extra.log_checks;
      stats.application_checks += extra.application_checks;
      stats.proposals += extra.proposals;
      stats.crashes += extra.crashes;
      stats.append_reply_crashes += extra.append_reply_crashes;
      stats.dropped += extra.dropped;
      digest = (digest ^ directed->Digest()) * 1099511628211ULL;
      commands += 1;
    }
    print_trace();
    return {.stats = stats,
            .commands = commands,
            .recovery = recovery + 1,
            .digest = digest,
            .figure8_runs = seed % 8 == 7 ? 1U : 0U,
            .ack_crash_runs = seed % 8 == 6 ? 1U : 0U};
  } catch (...) {
    print_trace();
    throw;
  }
}

TEST(ReplicationFuzz, CommittedHistorySurvivesSeededFaults) {
  const auto config = test::seed_config;
  Result totals;
  std::uint64_t detections = 0;
  std::uint64_t digest = 14695981039346656037ULL;
  for (std::uint64_t offset = 0; offset < config.count; ++offset) {
    const auto seed = config.first + offset;
    const auto replay =
        "quorumkv_tests --gtest_filter=ReplicationFuzz.* --seed=" + std::to_string(seed);
    try {
      const auto result = ReplicationScenario(seed, config.count == 1);
      totals.commands += result.commands;
      totals.figure8_runs += result.figure8_runs;
      totals.ack_crash_runs += result.ack_crash_runs;
      totals.recovery = std::max(totals.recovery, result.recovery);
      totals.stats.steps += result.stats.steps;
      totals.stats.safety_checks += result.stats.safety_checks;
      totals.stats.log_checks += result.stats.log_checks;
      totals.stats.application_checks += result.stats.application_checks;
      totals.stats.proposals += result.stats.proposals;
      totals.stats.crashes += result.stats.crashes;
      totals.stats.sync_failures += result.stats.sync_failures;
      totals.stats.candidate_crashes += result.stats.candidate_crashes;
      totals.stats.append_reply_crashes += result.stats.append_reply_crashes;
      totals.stats.long_delay_packets += result.stats.long_delay_packets;
      totals.stats.partitions += result.stats.partitions;
      totals.stats.dropped += result.stats.dropped;
      totals.stats.duplicated += result.stats.duplicated;
      digest = (digest ^ result.digest) * 1099511628211ULL;
    } catch (const std::logic_error& error) {
      if (test::mutation_survey) {
        ++detections;
        std::cout << "[mutation-detected] seed=" << seed << " invariant=" << error.what() << '\n';
        continue;
      }
      std::cerr << "[sim-failure] seed=" << seed << " replay=" << replay << '\n';
      if (config.count != 1) {
        try {
          static_cast<void>(ReplicationScenario(seed, true));
          FAIL() << "failure did not replay";
        } catch (const std::exception& replay_error) {
          EXPECT_STREQ(error.what(), replay_error.what());
        }
      }
      FAIL() << error.what() << "; seed=" << seed << "; replay: " << replay;
    } catch (const std::exception& error) {
      std::cerr << "[sim-failure] seed=" << seed << " replay=" << replay << '\n';
      if (config.count != 1) {
        try {
          static_cast<void>(ReplicationScenario(seed, true));
          FAIL() << "failure did not replay";
        } catch (const std::exception& replay_error) {
          EXPECT_STREQ(error.what(), replay_error.what());
        }
      }
      FAIL() << error.what();
    }
  }
  if (test::mutation_survey) {
    std::cout << "[mutation-survey] first_seed=" << config.first << " seeds=" << config.count
              << " detected=" << detections << '\n';
    EXPECT_GT(detections, 0U);
    return;
  }
  std::cout << "[replication] first_seed=" << config.first << " seeds=" << config.count
            << " steps=" << totals.stats.steps << " safety_checks=" << totals.stats.safety_checks
            << " log_checks=" << totals.stats.log_checks
            << " application_checks=" << totals.stats.application_checks
            << " proposals=" << totals.stats.proposals << " committed_commands=" << totals.commands
            << " figure8_runs=" << totals.figure8_runs
            << " ack_crash_runs=" << totals.ack_crash_runs << " crashes=" << totals.stats.crashes
            << " sync_failures=" << totals.stats.sync_failures
            << " candidate_crashes=" << totals.stats.candidate_crashes
            << " append_reply_crashes=" << totals.stats.append_reply_crashes
            << " long_delay_packets=" << totals.stats.long_delay_packets
            << " partitions=" << totals.stats.partitions << " dropped=" << totals.stats.dropped
            << " duplicated=" << totals.stats.duplicated
            << " max_recovery_ticks=" << totals.recovery << " digest=" << std::hex << digest
            << std::dec << '\n';
}

}  // namespace
}  // namespace quorumkv::sim
