#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "quorumkv/raft_node.hpp"

namespace quorumkv {

RaftNode::RaftNode(ElectionConfig config, Transport& transport, Clock& clock, Storage& storage,
                   Random& random)
    : config_(std::move(config)),
      transport_(transport),
      clock_(clock),
      storage_(storage),
      random_(random) {
  auto& members = config_.members;
  std::ranges::sort(members);
  if (members.empty() || !std::ranges::binary_search(members, config_.id) ||
      std::ranges::adjacent_find(members) != members.end() || config_.heartbeat_interval == 0 ||
      config_.election_min <= config_.heartbeat_interval ||
      config_.election_max < config_.election_min) {
    throw std::invalid_argument("invalid election configuration");
  }
  const auto state = storage_.Load();
  hard_state_ = state.hard_state;
  if (state.snapshot) {
    last_index_ = state.snapshot->last_included_index;
    last_term_ = state.snapshot->last_included_term;
  }
  if (!state.entries.empty()) {
    last_index_ = state.entries.back().index;
    last_term_ = state.entries.back().term;
  }
  ResetElectionTimer();
}

void RaftNode::EnsureRunning() const {
  if (stopped_) {
    throw std::runtime_error("node stopped after storage failure");
  }
}

void RaftNode::Save(HardState state) {
  StorageBatch batch;
  batch.hard_state = state;
  try {
    storage_.Persist(batch);
  } catch (...) {
    stopped_ = true;
    throw;
  }
  hard_state_ = state;
}

void RaftNode::ResetElectionTimer() {
  election_start_ = clock_.Now();
  election_timeout_ =
      config_.election_min + random_.Next() % (config_.election_max - config_.election_min + 1);
}

void RaftNode::Send(NodeId to, MessageBody body, std::uint64_t rpc_id) {
  transport_.Send(to, Message{.from = config_.id,
                              .to = to,
                              .term = hard_state_.term,
                              .rpc_id = rpc_id,
                              .body = std::move(body)});
}

void RaftNode::StartElection() {
  if (hard_state_.term == std::numeric_limits<Term>::max()) {
    stopped_ = true;
    throw std::overflow_error("term exhausted");
  }
  // The self-vote must survive a crash before any RequestVote leaves this node.
  Save({.term = hard_state_.term + 1, .voted_for = config_.id});
  role_ = Role::Candidate;
  leader_.reset();
  votes_ = {config_.id};
  ResetElectionTimer();
  if (votes_.size() > config_.members.size() / 2) {
    BecomeLeader();
    return;
  }
  for (const auto peer : config_.members) {
    if (peer != config_.id) {
      Send(peer, RequestVote{.last_log_index = last_index_, .last_log_term = last_term_});
    }
  }
}

void RaftNode::BecomeLeader() {
  role_ = Role::Leader;
  leader_ = config_.id;
  Heartbeat();
}

void RaftNode::Heartbeat() {
  last_heartbeat_ = clock_.Now();
  for (const auto peer : config_.members) {
    if (peer != config_.id) {
      Send(peer, AppendEntries{
                     .prev_log_index = last_index_, .prev_log_term = last_term_, .entries = {}});
    }
  }
}

void RaftNode::Tick() {
  EnsureRunning();
  const auto now = clock_.Now();
  if (role_ == Role::Leader) {
    if (now - last_heartbeat_ >= config_.heartbeat_interval) {
      Heartbeat();
    }
  } else if (now - election_start_ >= election_timeout_) {
    StartElection();
  }
}

void RaftNode::Step(const Message& message) {
  EnsureRunning();
  if (message.to != config_.id || message.from == config_.id ||
      !std::ranges::binary_search(config_.members, message.from)) {
    return;
  }
  if (message.term > hard_state_.term) {
    Save({.term = message.term, .voted_for = std::nullopt});
    role_ = Role::Follower;
    leader_.reset();
    votes_.clear();
    // A higher term alone must not postpone an election indefinitely.
  }
  if (const auto* vote = std::get_if<RequestVote>(&message.body)) {
    Receive(message, *vote);
  } else if (const auto* reply = std::get_if<VoteResponse>(&message.body)) {
    Receive(message, *reply);
  } else if (const auto* append = std::get_if<AppendEntries>(&message.body)) {
    Receive(message, *append);
  }
}

void RaftNode::Receive(const Message& message, const RequestVote& vote) {
  const bool up_to_date =
      std::tie(vote.last_log_term, vote.last_log_index) >= std::tie(last_term_, last_index_);
  const bool grant = message.term == hard_state_.term && up_to_date &&
                     (!hard_state_.voted_for || hard_state_.voted_for == message.from);
#if defined(QUORUMKV_TEST_REPLY_BEFORE_PERSIST)
  Send(message.from, VoteResponse{.granted = grant}, message.rpc_id);
#endif
  if (grant) {
    if (!hard_state_.voted_for) {
      Save({.term = hard_state_.term, .voted_for = message.from});
    }
    ResetElectionTimer();
  }
#if !defined(QUORUMKV_TEST_REPLY_BEFORE_PERSIST)
  Send(message.from, VoteResponse{.granted = grant}, message.rpc_id);
#endif
}

void RaftNode::Receive(const Message& message, const VoteResponse& vote) {
  const bool current_term =
#if defined(QUORUMKV_TEST_COUNT_STALE_VOTES)
      true;
#else
      message.term == hard_state_.term;
#endif
  if (current_term && role_ == Role::Candidate && vote.granted) {
    votes_.insert(message.from);
    if (votes_.size() > config_.members.size() / 2) {
      BecomeLeader();
    }
  }
}

void RaftNode::Receive(const Message& message, const AppendEntries& append) {
  bool success = false;
  if (message.term == hard_state_.term) {
    role_ = Role::Follower;
    leader_ = message.from;
    votes_.clear();
    ResetElectionTimer();
    // M2 only handles empty heartbeats at the local tail. Replication is M3.
    success = append.entries.empty() && append.prev_log_index == last_index_ &&
              append.prev_log_term == last_term_;
  }
  Send(message.from,
       AppendResponse{.success = success,
                      .match_index = success ? last_index_ : 0,
                      .conflict_term = std::nullopt,
                      .read_context = append.read_context},
       message.rpc_id);
}

}  // namespace quorumkv
