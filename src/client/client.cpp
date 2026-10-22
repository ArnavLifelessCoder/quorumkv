#include "quorumkv/client.hpp"

#include <random>
#include <stdexcept>
#include <thread>

#include "quorumkv/codec.hpp"

namespace quorumkv {
namespace {

using SteadyClock = std::chrono::steady_clock;

enum class Attempt : std::uint8_t {
  Answered,
  NotSent,    // nothing reached the node; the request had no effect there
  MaybeSent,  // the node may have received it; the outcome is unknown
};

struct Endpoint {
  net::Socket socket;
  net::FrameReader reader;
};

}  // namespace

struct Client::Impl {
  explicit Impl(ClientConfig client_config) : config(std::move(client_config)) {
    if (config.nodes.empty()) {
      throw std::invalid_argument("client needs at least one node");
    }
    net::Initialize();
    endpoints.resize(config.nodes.size());
  }

  ClientConfig config;
  std::vector<Endpoint> endpoints;
  std::size_t target{};
  std::uint64_t next_request{1};
  std::uint64_t sequence{};

  void Rotate() { target = (target + 1) % endpoints.size(); }

  void Follow(const std::optional<NodeId>& hint) {
    if (hint && *hint >= 1 && *hint <= endpoints.size()) {
      target = static_cast<std::size_t>(*hint - 1);
    } else {
      Rotate();
    }
  }

  // Waits until the socket is ready or the deadline passes.
  static auto Wait(const net::Socket& socket, bool write,
                   SteadyClock::time_point deadline) -> bool {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now());
    if (remaining.count() <= 0) {
      return false;
    }
    std::vector<net::PollEntry> entries{
        {.socket = &socket, .want_read = !write, .want_write = write}};
    net::Poll(entries, static_cast<int>(remaining.count()));
    return entries.front().readable || entries.front().writable || entries.front().failed;
  }

  auto Exchange(std::size_t index, const ClientRequest& request,
                ClientResponse& response) -> Attempt {
    auto& endpoint = endpoints.at(index);
    const auto deadline = SteadyClock::now() + config.attempt_timeout;
    if (!endpoint.socket.valid()) {
      endpoint = Endpoint{.socket = net::Connect(config.nodes.at(index)), .reader = {}};
      if (!endpoint.socket.valid() || !Wait(endpoint.socket, true, deadline) ||
          !net::Connected(endpoint.socket)) {
        endpoint = Endpoint{};
        return Attempt::NotSent;
      }
    }
    auto frame = EncodeClientRequest(request);
    const auto size = frame.size();
    while (!frame.empty()) {
      if (net::SendSome(endpoint.socket, frame) == net::IoResult::Closed ||
          (!frame.empty() && !Wait(endpoint.socket, true, deadline))) {
        endpoint = Endpoint{};
        return frame.size() == size ? Attempt::NotSent : Attempt::MaybeSent;
      }
    }
    for (;;) {
      try {
        while (auto bytes = endpoint.reader.Next()) {
          auto decoded = DecodeClientResponse(*bytes);
          // Answers to requests this client already gave up on are discarded.
          if (decoded.request_id == request.request_id) {
            response = std::move(decoded);
            return Attempt::Answered;
          }
        }
      } catch (const CodecError&) {
        endpoint = Endpoint{};
        return Attempt::MaybeSent;
      }
      if (!Wait(endpoint.socket, false, deadline)) {
        endpoint = Endpoint{};
        return Attempt::MaybeSent;
      }
      Bytes incoming;
      const auto result = net::Receive(endpoint.socket, incoming);
      endpoint.reader.Append(incoming);
      if (result == net::IoResult::Closed && incoming.empty()) {
        endpoint = Endpoint{};
        return Attempt::MaybeSent;
      }
    }
  }

  auto Run(ClientKind kind, const Bytes& payload, bool has_effect) -> Result {
    const auto deadline = SteadyClock::now() + config.operation_timeout;
    bool maybe_applied = false;
    std::size_t refusals = 0;
    while (SteadyClock::now() < deadline) {
      ClientResponse response;
      const ClientRequest request{.request_id = next_request++, .kind = kind, .payload = payload};
      const auto attempt = Exchange(target, request, response);
      if (attempt == Attempt::Answered && response.status == ClientStatus::Ok) {
        try {
          return {.outcome = Outcome::Ok, .response = kv::DecodeResponse(response.payload)};
        } catch (const CodecError&) {
          return {.outcome = has_effect ? Outcome::Unknown : Outcome::Fail,
                  .response = kv::Invalid{}};
        }
      }
      if (attempt == Attempt::MaybeSent ||
          (attempt == Attempt::Answered && response.status == ClientStatus::Retry)) {
        maybe_applied = true;
      }
      if (attempt == Attempt::Answered) {
        Follow(response.leader_hint);
      } else {
        Rotate();
      }
      // Back off once every node has refused, typically during an election.
      if (++refusals % endpoints.size() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    return {.outcome = has_effect && maybe_applied ? Outcome::Unknown : Outcome::Fail,
            .response = kv::Invalid{}};
  }
};

Client::Client(ClientConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Client::~Client() = default;

auto Client::Execute(kv::Operation operation) -> Result {
  const bool has_effect = !std::holds_alternative<kv::Get>(operation);
  const auto command = kv::EncodeRequest({.client = impl_->config.id,
                                          .sequence = ++impl_->sequence,
                                          .operation = std::move(operation)});
  return impl_->Run(ClientKind::Command, command, has_effect);
}

auto Client::Read(const std::string& key) -> Result {
  return impl_->Run(ClientKind::Read, kv::EncodeRead(key), false);
}

auto Client::Status(NodeId node) -> std::optional<NodeStatus> {
  if (node == 0 || node > impl_->endpoints.size()) {
    return std::nullopt;
  }
  ClientResponse response;
  const ClientRequest request{
      .request_id = impl_->next_request++, .kind = ClientKind::Status, .payload = {}};
  if (impl_->Exchange(static_cast<std::size_t>(node - 1), request, response) != Attempt::Answered ||
      response.status != ClientStatus::Ok) {
    return std::nullopt;
  }
  try {
    return DecodeNodeStatus(response.payload);
  } catch (const CodecError&) {
    return std::nullopt;
  }
}

}  // namespace quorumkv
