// quorumkv_load: closed-loop load generator for the benchmark drivers in bench/.
//
//   quorumkv_load --nodes A,B,C --mode write|read-index|read-log [--threads 32]
//                 [--duration-ms 10000] [--warmup-ms 2000] [--value-size 100] [--keys 1000]
//       One JSON line: throughput and latency percentiles over the measured window.
//   quorumkv_load --nodes A,B,C --mode probe [--duration-ms D]
//       One client writing back to back; prints "ok <invoke_ns> <complete_ns>" per success.
//   quorumkv_load --mode clock
//       Prints the steady clock in nanoseconds, to align with the driver's clock.
//
// Every thread owns its own Client and connections, so threads share no state except the
// atomic stop flag and the result vectors they fill before joining.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "quorumkv/client.hpp"

namespace {

using Steady = std::chrono::steady_clock;

auto Number(std::string_view text) -> std::uint64_t {
  std::uint64_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument("expected a number, got " + std::string(text));
  }
  return value;
}

auto Nanoseconds() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Steady::now().time_since_epoch())
      .count();
}

struct Options {
  std::vector<quorumkv::net::Address> nodes;
  std::string mode = "write";
  std::uint64_t threads = 32;
  std::uint64_t duration_ms = 10000;
  std::uint64_t warmup_ms = 2000;
  std::uint64_t value_size = 100;
  std::uint64_t keys = 1000;
};

auto Parse(int argc, char** argv) -> Options {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(flag));
    }
    const std::string value(argv[++index]);
    if (flag == "--nodes") {
      std::string_view text(value);
      while (!text.empty()) {
        const auto comma = text.find(',');
        options.nodes.push_back(quorumkv::net::ParseAddress(std::string(text.substr(0, comma))));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
      }
    } else if (flag == "--mode") {
      options.mode = value;
    } else if (flag == "--threads") {
      options.threads = Number(value);
    } else if (flag == "--duration-ms") {
      options.duration_ms = Number(value);
    } else if (flag == "--warmup-ms") {
      options.warmup_ms = Number(value);
    } else if (flag == "--value-size") {
      options.value_size = Number(value);
    } else if (flag == "--keys") {
      options.keys = Number(value);
    } else {
      throw std::invalid_argument("unknown flag " + std::string(flag));
    }
  }
  return options;
}

auto MakeClient(const Options& options, std::uint64_t thread) -> std::unique_ptr<quorumkv::Client> {
  std::random_device device;
  return std::make_unique<quorumkv::Client>(quorumkv::ClientConfig{
      .nodes = options.nodes,
      .id = {.high = (static_cast<std::uint64_t>(device()) << 32U) | device(), .low = thread},
      .attempt_timeout = std::chrono::milliseconds(1000),
      .operation_timeout = std::chrono::milliseconds(10000)});
}

struct ThreadResult {
  std::vector<std::int64_t> latencies_ns;
  std::uint64_t failed{};
  std::uint64_t unknown{};
};

auto Percentile(std::vector<std::int64_t>& values, double fraction) -> double {
  if (values.empty()) {
    return 0;
  }
  const auto rank = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(rank),
                   values.end());
  return static_cast<double>(values[rank]) / 1000.0;
}

auto RunLoad(const Options& options) -> int {
  if (options.mode != "write" && options.mode != "read-index" && options.mode != "read-log") {
    throw std::invalid_argument("unknown mode " + options.mode);
  }
  const std::string value(options.value_size, 'v');
  // Every key exists before reads are measured.
  if (options.mode != "write") {
    auto client = MakeClient(options, 0);
    for (std::uint64_t key = 0; key < options.keys; ++key) {
      client->Execute(quorumkv::kv::Put{.key = "k" + std::to_string(key), .value = value});
    }
  }
  const auto start = Steady::now();
  const auto measure_from = start + std::chrono::milliseconds(options.warmup_ms);
  const auto end = measure_from + std::chrono::milliseconds(options.duration_ms);
  std::vector<ThreadResult> results(options.threads);
  std::vector<std::thread> threads;
  threads.reserve(options.threads);
  for (std::uint64_t thread = 0; thread < options.threads; ++thread) {
    threads.emplace_back([&, thread] {
      auto client = MakeClient(options, thread + 1);
      std::mt19937_64 random(thread * 0x9e3779b97f4a7c15ULL + 1);
      auto& result = results[thread];
      while (Steady::now() < end) {
        const auto key = "k" + std::to_string(random() % options.keys);
        const auto invoked = Steady::now();
        quorumkv::Result outcome;
        if (options.mode == "write") {
          outcome = client->Execute(quorumkv::kv::Put{.key = key, .value = value});
        } else if (options.mode == "read-index") {
          outcome = client->Read(key);
        } else {
          outcome = client->Execute(quorumkv::kv::Get{.key = key});
        }
        const auto completed = Steady::now();
        if (invoked < measure_from || completed > end) {
          continue;  // Only operations entirely inside the measured window count.
        }
        if (outcome.outcome == quorumkv::Outcome::Ok) {
          result.latencies_ns.push_back((completed - invoked).count());
        } else if (outcome.outcome == quorumkv::Outcome::Fail) {
          ++result.failed;
        } else {
          ++result.unknown;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  std::vector<std::int64_t> latencies;
  std::uint64_t failed = 0;
  std::uint64_t unknown = 0;
  for (auto& result : results) {
    latencies.insert(latencies.end(), result.latencies_ns.begin(), result.latencies_ns.end());
    failed += result.failed;
    unknown += result.unknown;
  }
  const auto seconds = static_cast<double>(options.duration_ms) / 1000.0;
  const auto ok = latencies.size();
  std::cout << R"({"mode":")" << options.mode << R"(","threads":)" << options.threads
            << R"(,"value_size":)" << options.value_size << R"(,"duration_s":)" << seconds
            << R"(,"ok":)" << ok << R"(,"fail":)" << failed << R"(,"unknown":)" << unknown
            << R"(,"ops_per_sec":)" << static_cast<double>(ok) / seconds << R"(,"p50_us":)"
            << Percentile(latencies, 0.5) << R"(,"p99_us":)" << Percentile(latencies, 0.99)
            << R"(,"max_us":)" << Percentile(latencies, 1.0) << "}\n";
  return 0;
}

auto RunProbe(const Options& options) -> int {
  auto client = MakeClient(options, 1);
  const auto end = Steady::now() + std::chrono::milliseconds(options.duration_ms);
  for (std::uint64_t sequence = 1; Steady::now() < end; ++sequence) {
    const auto invoked = Nanoseconds();
    const auto outcome =
        client->Execute(quorumkv::kv::Put{.key = "probe", .value = std::to_string(sequence)});
    if (outcome.outcome == quorumkv::Outcome::Ok) {
      std::cout << "ok " << invoked << ' ' << Nanoseconds() << '\n';
      std::cout.flush();
    }
  }
  return 0;
}

auto Run(int argc, char** argv) -> int {
  const auto options = Parse(argc, argv);
  if (options.mode == "clock") {
    std::cout << Nanoseconds() << '\n';
    return 0;
  }
  if (options.nodes.empty() || options.threads == 0 || options.keys == 0) {
    throw std::invalid_argument("--nodes, threads, and keys are required");
  }
  return options.mode == "probe" ? RunProbe(options) : RunLoad(options);
}

}  // namespace

auto main(int argc, char** argv) -> int try {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "quorumkv_load: " << error.what() << '\n';
    return 1;
  }
} catch (...) {
  return 1;
}
