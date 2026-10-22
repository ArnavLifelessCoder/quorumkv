// quorumkv_client: talk to a quorumkv cluster.
//
//   quorumkv_client --nodes 127.0.0.1:7201,127.0.0.1:7202,127.0.0.1:7203 COMMAND
//
// Commands:
//   status                         one JSON line per node
//   put KEY VALUE | append KEY SUFFIX | cas KEY EXPECTED NEW | get KEY | logged-get KEY
//   workload --client N --seed S --duration-ms D [--keys K] --out FILE
//       Runs random operations and writes one JSON line per operation:
//       {"client","op","key","value","expected","invoke_ts","complete_ts","result","output"}.
//       Timestamps are nanoseconds on the system-wide monotonic clock, so histories from
//       several client processes on one machine share one time line. `result` is "ok",
//       "fail" (definitely had no effect), or "info" (may or may not have taken effect).

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "quorumkv/client.hpp"

namespace {

using quorumkv::Outcome;

auto Number(std::string_view text) -> std::uint64_t {
  std::uint64_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument("expected a number, got " + std::string(text));
  }
  return value;
}

auto Quote(std::string_view text) -> std::string {
  std::string out = R"(")";
  for (const char character : text) {
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          static constexpr std::string_view kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex.at(static_cast<std::size_t>((character >> 4) & 0xf));
          out += kHex.at(static_cast<std::size_t>(character & 0xf));
        } else {
          out += character;
        }
    }
  }
  return out + '"';
}

auto Name(Outcome outcome) -> const char* {
  switch (outcome) {
    case Outcome::Ok:
      return "ok";
    case Outcome::Fail:
      return "fail";
    case Outcome::Unknown:
      return "info";
  }
  return "info";
}

// JSON for the result fields of a successful response.
auto Output(const quorumkv::kv::Response& response) -> std::string {
  return std::visit(
      [](const auto& result) -> std::string {
        using T = std::decay_t<decltype(result)>;
        if constexpr (std::is_same_v<T, quorumkv::kv::Value>) {
          return std::string(R"({"found":)") + (result.found ? "true" : "false") + R"(,"value":)" +
                 Quote(result.value) + "}";
        } else if constexpr (std::is_same_v<T, quorumkv::kv::CasResult>) {
          return std::string(R"({"exchanged":)") + (result.exchanged ? "true" : "false") +
                 R"(,"found":)" + (result.found ? "true" : "false") + R"(,"previous":)" +
                 Quote(result.previous) + "}";
        } else {
          return "{}";
        }
      },
      response);
}

auto Nanoseconds() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

auto Nodes(std::string_view text) -> std::vector<quorumkv::net::Address> {
  std::vector<quorumkv::net::Address> nodes;
  while (!text.empty()) {
    const auto comma = text.find(',');
    nodes.push_back(quorumkv::net::ParseAddress(std::string(text.substr(0, comma))));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
  }
  return nodes;
}

auto Identity(std::uint64_t client) -> quorumkv::kv::ClientId {
  std::random_device device;
  // A fresh high word per process keeps restarted clients from reusing an old session.
  return {.high = (static_cast<std::uint64_t>(device()) << 32U) | device(), .low = client};
}

void Status(quorumkv::Client& client, std::size_t count) {
  for (quorumkv::NodeId id = 1; id <= count; ++id) {
    const auto status = client.Status(id);
    std::cout << R"({"id":)" << id;
    if (status) {
      static constexpr std::array<std::string_view, 3> kRoles{"follower", "candidate", "leader"};
      std::cout << R"(,"up":true,"role":")" << kRoles.at(static_cast<std::size_t>(status->role))
                << R"(","term":)" << status->term << R"(,"leader":)" << status->leader.value_or(0)
                << R"(,"commit":)" << status->commit_index << R"(,"applied":)"
                << status->last_applied << R"(,"snapshot":)" << status->snapshot_index;
    } else {
      std::cout << R"(,"up":false)";
    }
    std::cout << "}\n";
  }
}

auto Workload(std::vector<quorumkv::net::Address> nodes, int argc, char** argv, int index) -> int {
  std::uint64_t client_number = 1;
  std::uint64_t seed = 1;
  std::uint64_t duration_ms = 10000;
  std::uint64_t keys = 3;
  std::string out;
  for (; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(flag));
    }
    const std::string value(argv[++index]);
    if (flag == "--client") {
      client_number = Number(value);
    } else if (flag == "--seed") {
      seed = Number(value);
    } else if (flag == "--duration-ms") {
      duration_ms = Number(value);
    } else if (flag == "--keys") {
      keys = Number(value);
    } else if (flag == "--out") {
      out = value;
    } else {
      throw std::invalid_argument("unknown workload flag " + std::string(flag));
    }
  }
  if (out.empty() || keys == 0) {
    throw std::invalid_argument("workload requires --out and at least one key");
  }
  std::ofstream history(out, std::ios::binary | std::ios::trunc);
  quorumkv::Client client({.nodes = std::move(nodes), .id = Identity(client_number)});
  std::mt19937_64 random(seed ^ (client_number * 0x9e3779b97f4a7c15ULL));
  std::map<std::string, std::string> seen;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  for (std::uint64_t operation = 1; std::chrono::steady_clock::now() < end; ++operation) {
    const auto key = "k" + std::to_string(random() % keys);
    const auto token = "c" + std::to_string(client_number) + "-" + std::to_string(operation);
    const auto roll = random() % 100;
    std::string op;
    std::string value;
    std::string expected;
    const auto invoke = Nanoseconds();
    quorumkv::Result result;
    if (roll < 25) {
      op = "put";
      value = token;
      result = client.Execute(quorumkv::kv::Put{.key = key, .value = value});
    } else if (roll < 45) {
      op = "append";
      value = token + ";";
      result = client.Execute(quorumkv::kv::Append{.key = key, .suffix = value});
    } else if (roll < 60) {
      op = "cas";
      value = token;
      expected = seen.contains(key) ? seen[key] : "";
      result =
          client.Execute(quorumkv::kv::Cas{.key = key, .expected = expected, .replacement = value});
    } else {
      op = "get";
      result = client.Read(key);
    }
    const auto complete = Nanoseconds();
    if (const auto* read = std::get_if<quorumkv::kv::Value>(&result.response);
        read != nullptr && result.outcome == Outcome::Ok && read->found) {
      seen[key] = read->value;
    }
    history << R"({"client":)" << client_number << R"(,"op":")" << op << R"(","key":)" << Quote(key)
            << R"(,"value":)" << (op == "get" ? "null" : Quote(value)) << R"(,"expected":)"
            << (op == "cas" ? Quote(expected) : "null") << R"(,"invoke_ts":)" << invoke
            << R"(,"complete_ts":)" << complete << R"(,"result":")" << Name(result.outcome)
            << R"(","output":)" << (result.outcome == Outcome::Ok ? Output(result.response) : "{}")
            << "}\n";
    history.flush();
  }
  return 0;
}

auto Run(int argc, char** argv) -> int {
  if (argc < 4 || std::string_view(argv[1]) != "--nodes") {
    std::cerr << "usage: quorumkv_client --nodes host:port,... COMMAND [ARGS]\n";
    return 2;
  }
  auto nodes = Nodes(argv[2]);
  const std::string_view command(argv[3]);
  if (command == "workload") {
    return Workload(std::move(nodes), argc, argv, 4);
  }
  const auto count = nodes.size();
  quorumkv::Client client({.nodes = std::move(nodes), .id = Identity(0)});
  if (command == "status") {
    Status(client, count);
    return 0;
  }
  const auto argument = [&](int position) -> std::string {
    if (argc <= position) {
      throw std::invalid_argument("missing argument for " + std::string(command));
    }
    return argv[position];
  };
  quorumkv::Result result;
  if (command == "put") {
    result = client.Execute(quorumkv::kv::Put{.key = argument(4), .value = argument(5)});
  } else if (command == "append") {
    result = client.Execute(quorumkv::kv::Append{.key = argument(4), .suffix = argument(5)});
  } else if (command == "cas") {
    result = client.Execute(
        quorumkv::kv::Cas{.key = argument(4), .expected = argument(5), .replacement = argument(6)});
  } else if (command == "get") {
    result = client.Read(argument(4));
  } else if (command == "logged-get") {
    result = client.Execute(quorumkv::kv::Get{.key = argument(4)});
  } else {
    throw std::invalid_argument("unknown command " + std::string(command));
  }
  std::cout << R"({"result":")" << Name(result.outcome) << R"(","output":)"
            << (result.outcome == Outcome::Ok ? Output(result.response) : "{}") << "}\n";
  return result.outcome == Outcome::Ok ? 0 : 1;
}

}  // namespace

auto main(int argc, char** argv) -> int try {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "quorumkv_client: " << error.what() << '\n';
    return 1;
  }
} catch (...) {
  // Reporting itself failed; the exit status still says the run failed.
  return 1;
}
