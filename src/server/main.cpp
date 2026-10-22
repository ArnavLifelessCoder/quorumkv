// quorumkv_server: one Raft KV node over TCP.
//
//   quorumkv_server --id 1 --members 1,2,3 --peer-listen 127.0.0.1:7101
//                   --client-listen 127.0.0.1:7201 --peer 2=127.0.0.1:7302
//                   --peer 3=127.0.0.1:7303 --data data/n1 [--tick-ms 10]
//                   [--snapshot-threshold 1000] [--unsafe-reads]
//
// Prints "ready id=<id> peer=<port> client=<port>" once both listeners are bound.

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "quorumkv/server.hpp"

namespace {

auto Number(std::string_view text) -> std::uint64_t {
  std::uint64_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument("expected a number, got " + std::string(text));
  }
  return value;
}

auto Members(std::string_view text) -> std::vector<quorumkv::NodeId> {
  std::vector<quorumkv::NodeId> members;
  while (!text.empty()) {
    const auto comma = text.find(',');
    members.push_back(Number(text.substr(0, comma)));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
  }
  return members;
}

auto Run(int argc, char** argv) -> int {
  quorumkv::ServerConfig config;
  std::map<quorumkv::NodeId, quorumkv::net::Address> peers;
  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    const auto value = [&]() -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for " + std::string(flag));
      }
      return argv[++index];
    };
    if (flag == "--id") {
      config.id = Number(value());
    } else if (flag == "--members") {
      config.members = Members(value());
    } else if (flag == "--peer-listen") {
      config.peer_listen = quorumkv::net::ParseAddress(value());
    } else if (flag == "--client-listen") {
      config.client_listen = quorumkv::net::ParseAddress(value());
    } else if (flag == "--peer") {
      const auto text = value();
      const auto equals = text.find('=');
      if (equals == std::string::npos) {
        throw std::invalid_argument("--peer expects id=host:port");
      }
      peers[Number(std::string_view(text).substr(0, equals))] =
          quorumkv::net::ParseAddress(text.substr(equals + 1));
    } else if (flag == "--data") {
      config.data_dir = value();
    } else if (flag == "--tick-ms") {
      config.tick_ms = static_cast<unsigned>(Number(value()));
    } else if (flag == "--snapshot-threshold") {
      config.snapshot_threshold = Number(value());
    } else if (flag == "--unsafe-reads") {
      config.unsafe_reads = true;
    } else {
      throw std::invalid_argument("unknown flag " + std::string(flag));
    }
  }
  if (config.id == 0 || config.members.empty() || config.data_dir.empty() || config.tick_ms == 0) {
    throw std::invalid_argument("--id, --members, and --data are required");
  }
  std::filesystem::create_directories(config.data_dir);
  quorumkv::Server server(config);
  server.SetPeers(std::move(peers));
  std::cout << "ready id=" << config.id << " peer=" << server.peer_port()
            << " client=" << server.client_port() << '\n';
  // The harness waits for this line, so it must not sit in a buffer.
  std::cout.flush();
  const std::atomic<bool> stop{false};
  server.Run(stop);
  return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int try {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "quorumkv_server: " << error.what() << '\n';
    return 1;
  }
} catch (...) {
  // Reporting itself failed; the exit status still says the run failed.
  return 1;
}
