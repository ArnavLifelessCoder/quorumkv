#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "quorumkv/codec.hpp"
#include "quorumkv/raft_node.hpp"

namespace quorumkv {

RaftNode::RaftNode(ElectionConfig config, Transport& transport, Clock& clock, Storage& storage,
                   Random& random, StateMachine* machine)
    : config_(std::move(config)),
      transport_(transport),
      clock_(clock),
      storage_(storage),
      random_(random),
      machine_(machine) {
  auto& members = config_.members;
  std::ranges::sort(members);
  if (members.empty() || !std::ranges::binary_search(members, config_.id) ||
      std::ranges::adjacent_find(members) != members.end() || config_.heartbeat_interval == 0 ||
      config_.election_min <= config_.heartbeat_interval ||
      config_.election_max < config_.election_min || config_.max_append_entries == 0 ||
      config_.max_append_entries > kMaxEntries) {
    throw std::invalid_argument("invalid election configuration");
  }
  persistent_ = storage_.Load();
  hard_state_ = persistent_.hard_state;
  log_ = RaftLog(persistent_);
  commit_index_ = log_.base_index();
  last_applied_ = log_.base_index();
  ResetElectionTimer();
}

void RaftNode::ApplyCommitted() {
  // Only committed entries are applied, once each and in log order.
  while (last_applied_ < commit_index_) {
    const auto& entry = log_.at(last_applied_ + 1);
    ++last_applied_;
    if (machine_ != nullptr && entry.kind == EntryKind::Command) {
      applied_.push_back(
          {.index = entry.index, .term = entry.term, .response = machine_->Apply(entry.command)});
    }
  }
}

auto RaftNode::ReadConfirmed(std::uint64_t context) const -> bool {
  std::size_t acknowledgements = 1;
  for (const auto& [peer, progress] : progress_) {
    static_cast<void>(peer);
    if (progress.acked_context >= context) {
      ++acknowledgements;
    }
  }
  return acknowledgements > config_.members.size() / 2;
}

void RaftNode::ServeReads() {
  if (role_ != Role::Leader) {
    return;
  }
  const bool current_term_committed = log_.term(commit_index_) == hard_state_.term;
  std::vector<PendingRead> waiting;
  for (auto& read : reads_) {
    // A new leader learns the commit point only after committing an entry of its own term.
    if (!read.read_index && current_term_committed) {
      read.read_index = commit_index_;
    }
    if (read.read_index && last_applied_ >= *read.read_index && ReadConfirmed(read.context)) {
      read_results_.push_back(
          {.id = read.id,
           .ok = true,
           .response = machine_ != nullptr ? machine_->Read(read.query) : Bytes{}});
    } else {
      waiting.push_back(std::move(read));
    }
  }
  reads_ = std::move(waiting);
}

void RaftNode::FailReads() {
  for (const auto& read : reads_) {
    read_results_.push_back({.id = read.id, .ok = false, .response = {}});
  }
  reads_.clear();
}

auto RaftNode::Read(std::uint64_t id, Bytes query) -> bool {
  EnsureRunning();
  if (config_.unsafe_local_reads) {
    read_results_.push_back(
        {.id = id, .ok = true, .response = machine_ != nullptr ? machine_->Read(query) : Bytes{}});
    return true;
  }
  if (role_ != Role::Leader) {
    return false;
  }
  if (read_context_ == std::numeric_limits<std::uint64_t>::max()) {
    stopped_ = true;
    throw std::overflow_error("read context exhausted");
  }
  reads_.push_back({.id = id,
                    .query = std::move(query),
                    .context = ++read_context_,
                    .read_index = std::nullopt});
  // The heartbeat round carries the new context; only replies to it confirm leadership.
  Heartbeat();
  ServeReads();
  return true;
}

auto RaftNode::TakeApplied() -> std::vector<Applied> { return std::exchange(applied_, {}); }

auto RaftNode::TakeReads() -> std::vector<ReadResult> { return std::exchange(read_results_, {}); }

void RaftNode::EnsureRunning() const {
  if (stopped_) {
    throw std::runtime_error("node stopped after storage failure");
  }
}

void RaftNode::Save(HardState state) {
  StorageBatch batch;
  batch.hard_state = state;
  Persist(batch);
}

void RaftNode::Persist(const StorageBatch& batch) {
  try {
    auto next = ApplyStorageBatch(persistent_, batch);
    storage_.Persist(batch);
    persistent_ = std::move(next);
    hard_state_ = persistent_.hard_state;
    log_ = RaftLog(persistent_);
  } catch (...) {
    stopped_ = true;
    throw;
  }
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
      Send(peer,
           RequestVote{.last_log_index = log_.last_index(), .last_log_term = log_.last_term()});
    }
  }
}

void RaftNode::BecomeLeader() {
  const auto no_op = AppendLocal(EntryKind::NoOp, {});
  role_ = Role::Leader;
  leader_ = config_.id;
  progress_.clear();
  for (const auto peer : config_.members) {
    if (peer != config_.id) {
      progress_.try_emplace(peer, no_op);
    }
  }
  AdvanceCommit();
  Heartbeat();
}

auto RaftNode::AppendLocal(EntryKind kind, Bytes command) -> LogIndex {
  if (log_.last_index() == std::numeric_limits<LogIndex>::max()) {
    stopped_ = true;
    throw std::overflow_error("log index exhausted");
  }
  const auto index = log_.last_index() + 1;
  StorageBatch batch;
  batch.append.push_back(
      {.index = index, .term = hard_state_.term, .kind = kind, .command = std::move(command)});
  Persist(batch);
  return index;
}

auto RaftNode::Propose(Bytes command) -> std::optional<LogIndex> {
  EnsureRunning();
  if (role_ != Role::Leader) {
    return std::nullopt;
  }
  if (command.size() > kMaxBlobBytes) {
    throw std::invalid_argument("command exceeds peer codec limit");
  }
  const auto index = AppendLocal(EntryKind::Command, std::move(command));
  AdvanceCommit();
  Heartbeat();
  ApplyCommitted();
  ServeReads();
  return index;
}

void RaftNode::SendAppend(NodeId peer) {
  auto& progress = progress_.at(peer);
  if (progress.next <= log_.base_index()) {
    return;  // Snapshot transfer is introduced in M5.
  }
  auto entries = log_.slice(progress.next, config_.max_append_entries);
  std::size_t bytes = 128;
  std::size_t count = 0;
  for (const auto& entry : entries) {
    if (entry.command.size() + 32 > kMaxFrameBytes - bytes) {
      break;
    }
    bytes += entry.command.size() + 32;
    ++count;
  }
  entries.resize(count);
  const auto previous = progress.next - 1;
  const auto last = entries.empty() ? previous : entries.back().index;
  if (rpc_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    stopped_ = true;
    throw std::overflow_error("RPC sequence exhausted");
  }
  const auto rpc = ++rpc_sequence_;
  // Bound retained correlation state even when every response is lost.
  if (progress.flights.size() == 64) {
    progress.flights.erase(progress.flights.begin());
  }
  progress.flights.emplace(rpc, Flight{.previous = previous, .last = last});
  // Speculatively send subsequent batches while earlier RPCs are in flight.
  // Only successful correlated responses advance match; rejection rewinds next.
  progress.next = last + 1;
  Send(peer,
       AppendEntries{.prev_log_index = previous,
                     .prev_log_term = log_.term(previous),
                     .leader_commit = commit_index_,
                     .read_context = read_context_,
                     .entries = std::move(entries)},
       rpc);
}

auto RaftNode::AdvanceCommit() -> bool {
  std::vector<LogIndex> matches{log_.last_index()};
  for (const auto& [peer, progress] : progress_) {
    static_cast<void>(peer);
    matches.push_back(progress.match);
  }
  std::ranges::sort(matches);
  const auto candidate = matches.at((matches.size() - 1) / 2);
  if (candidate > commit_index_ && log_.term(candidate) == hard_state_.term) {
    commit_index_ = candidate;
    return true;
  }
  return false;
}

void RaftNode::Heartbeat() {
  last_heartbeat_ = clock_.Now();
  for (const auto peer : config_.members) {
    if (peer != config_.id) {
      SendAppend(peer);
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
  ApplyCommitted();
  ServeReads();
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
    progress_.clear();
    FailReads();
    // A higher term alone must not postpone an election indefinitely.
  }
  if (const auto* vote = std::get_if<RequestVote>(&message.body)) {
    Receive(message, *vote);
  } else if (const auto* reply = std::get_if<VoteResponse>(&message.body)) {
    Receive(message, *reply);
  } else if (const auto* append = std::get_if<AppendEntries>(&message.body)) {
    Receive(message, *append);
  } else if (const auto* append_reply = std::get_if<AppendResponse>(&message.body)) {
    Receive(message, *append_reply);
  }
  ApplyCommitted();
  ServeReads();
}

void RaftNode::Receive(const Message& message, const RequestVote& vote) {
  const bool up_to_date = std::pair{vote.last_log_term, vote.last_log_index} >=
                          std::pair{log_.last_term(), log_.last_index()};
  const bool grant = message.term == hard_state_.term && up_to_date &&
                     (!hard_state_.voted_for || hard_state_.voted_for == message.from);
  if (grant) {
    if (!hard_state_.voted_for) {
      Save({.term = hard_state_.term, .voted_for = message.from});
    }
    ResetElectionTimer();
  }
  Send(message.from, VoteResponse{.granted = grant}, message.rpc_id);
}

void RaftNode::Receive(const Message& message, const VoteResponse& vote) {
  const bool current_term = message.term == hard_state_.term;
  if (current_term && role_ == Role::Candidate && vote.granted) {
    votes_.insert(message.from);
    if (votes_.size() > config_.members.size() / 2) {
      BecomeLeader();
    }
  }
}

void RaftNode::Receive(const Message& message, const AppendEntries& append) {
  AppendResponse reply{.conflict_term = std::nullopt, .read_context = append.read_context};
  const auto respond = [&] { Send(message.from, reply, message.rpc_id); };
  if (message.term != hard_state_.term) {
    respond();
    return;
  }
  role_ = Role::Follower;
  leader_ = message.from;
  votes_.clear();
  progress_.clear();
  FailReads();
  ResetElectionTimer();
  if (append.prev_log_index < log_.base_index()) {
    reply.conflict_index = log_.base_index() + 1;
    respond();
    return;
  }
  if (append.prev_log_index > log_.last_index()) {
    reply.conflict_index = log_.last_index() + 1;
    respond();
    return;
  }
  if (log_.term(append.prev_log_index) != append.prev_log_term) {
    reply.conflict_term = log_.term(append.prev_log_index);
    reply.conflict_index = log_.first_in_term(append.prev_log_index);
    respond();
    return;
  }
  auto expected = append.prev_log_index;
  auto term = append.prev_log_term;
  for (const auto& entry : append.entries) {
    if (expected == std::numeric_limits<LogIndex>::max() || entry.index != expected + 1 ||
        entry.term == 0 || entry.term < term || entry.term > message.term ||
        entry.command.size() > kMaxBlobBytes ||
        (entry.kind != EntryKind::NoOp && entry.kind != EntryKind::Command) ||
        (entry.kind == EntryKind::NoOp && !entry.command.empty())) {
      respond();
      return;
    }
    expected = entry.index;
    term = entry.term;
  }
  StorageBatch batch;
  for (std::size_t offset = 0; offset < append.entries.size(); ++offset) {
    const auto& entry = append.entries[offset];
    if (entry.index > log_.last_index() || log_.term(entry.index) != entry.term) {
      if (entry.index <= commit_index_) {
        stopped_ = true;
        throw std::logic_error("attempt to replace a committed log entry");
      }
      if (entry.index <= log_.last_index()) {
        batch.truncate_from = entry.index;
      }
      batch.append.assign(append.entries.begin() + static_cast<std::ptrdiff_t>(offset),
                          append.entries.end());
      break;
    }
  }
  if (batch.truncate_from || !batch.append.empty()) {
    Persist(batch);
  }
  commit_index_ = std::max(commit_index_, std::min(append.leader_commit, expected));
  reply.success = true;
  reply.match_index = expected;
  respond();
}

void RaftNode::Receive(const Message& message, const AppendResponse& reply) {
  if (role_ != Role::Leader || message.term != hard_state_.term) {
    return;
  }
  auto& progress = progress_.at(message.from);
  // Any current-term reply to a heartbeat sent after a read was registered shows this
  // peer had not moved to a newer term, so it counts toward that read's leadership check.
  progress.acked_context = std::max(progress.acked_context, reply.read_context);
  const auto found = progress.flights.find(message.rpc_id);
  if (found == progress.flights.end()) {
    return;
  }
  const auto flight = found->second;
  progress.flights.erase(found);
  if (reply.success) {
    if (reply.match_index != flight.last) {
      return;
    }
    progress.match = std::max(progress.match, flight.last);
    progress.next = std::max(progress.next, progress.match + 1);
    std::erase_if(progress.flights,
                  [&](const auto& item) { return item.second.last <= progress.match; });
    if (AdvanceCommit()) {
      Heartbeat();
    } else if (progress.match < log_.last_index()) {
      SendAppend(message.from);
    }
  } else if (flight.previous > progress.match) {
    auto next = reply.conflict_index;
    if (reply.conflict_term) {
      if (const auto last = log_.last_in_term(*reply.conflict_term)) {
        next = *last + 1;
      }
    }
    next = std::max(progress.match + 1, std::min(next, flight.previous));
    progress.next = std::min(progress.next, next);
    SendAppend(message.from);
  }
}

}  // namespace quorumkv
