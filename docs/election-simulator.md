# Election core and deterministic simulator

## Core boundary

`RaftNode` owns election state and takes exactly four injected runtime dependencies:
Transport, Clock, Storage, and Random. It has no socket, thread, wall-clock, or
global random calls. All calls are serialized by its driver. Transport must copy
and enqueue messages, never call back synchronously.

Construction loads durable hard state and the local log/snapshot tail. The
candidate freshness comparison is lexicographic `(last_term, last_index)`.
An election increments the term and persists the self-vote before broadcasting.
A majority counts distinct configured members, including self. Messages from
unknown senders, self, or addressed to another node cannot change state.

Higher terms clear the durable vote and step the node down before processing or
replying. Granting a vote and receiving a current-term heartbeat reset the
election timer. Denied votes, stale heartbeats, and a higher term alone do not.
A same-term heartbeat steps a candidate down without clearing its vote.
The node stops on a failed persistence call and cannot continue through later
Step/Tick calls. The driver must destroy and reconstruct it from durable storage.

M2 heartbeats are empty AppendEntries at the local tail. A matching heartbeat
gets a success reply; a mismatch or nonempty append is rejected without changing
the log. Current-term leader contact resets the election timeout even when the
log check fails. No-op insertion, log replication, conflict backup, commit
advancement, disk adapters, and snapshot installation belong to M3/M5.

## Simulation events

There is one virtual clock and one serialized driver. Each node has its own
SplitMix64 stream. A separate stream chooses message faults, delays, and tick
ordering; the campaign has another stream for its fault schedule. All arithmetic
is fixed-width unsigned arithmetic. The implementation does not use standard
library random distributions, unordered-container iteration, or physical time.

One `Advance()` call advances virtual time, ticks live nodes in seeded order, then
delivers due packets. The queue is ordered by `(delivery_tick, sequence)`, so ties
are stable. Delay is at least one tick; variable delays naturally reorder traffic.
Duplicates have separate delivery times. Packets can survive a sender crash;
delivery to a crashed recipient drops them. Partition labels are checked both
when enqueueing and delivering. In-flight packets may cross a healed link.

A simulated step is a clock advance, one complete node Tick/Step, dropped delivery,
crash, restart, or fault/partition change. A node call has no interleaved callback,
but can be interrupted at a persistence or transport boundary by a modeled crash.
The historical safety oracle runs after every such step, including these crashes.

The oracle checks:

- At most one distinct node has ever been leader in each term, including leaders
  that subsequently stepped down or crashed.
- A node's durable term never decreases and its vote cannot change or disappear
  within a term.
- Every live node's hard state equals its durable storage.

An isolated old leader and a majority leader may coexist in different terms.
A directed test covers this case and their convergence after healing. Separate
negative tests prove the oracle rejects conflicting historical leaders, regressed
terms, and lost/changed votes.

## Crash model

MemoryStorage models pending and durable hard state separately. A before-sync
failure throws with only a pending write; a crash discards it. An after-sync
failure throws after publishing the durable state. Both stop the node before it
can send a reply. Restart reconstructs the node and its volatile votes/timers
from the durable image. The random stream continues deterministically.

The transport can also cut power immediately after queuing a granted vote reply.
It throws an internal power-loss exception; the driver destroys the node without
allowing the remainder of `Step` to execute. It never calls back into the node.
The queued reply survives the sender's crash. Correct ordering leaves a durable
vote; reply-before-persist leaves no vote, allowing a restarted node to vote again.
The simulator does not assert persistence ordering in `Send`: the mutation is
caught by the historical leader invariant after the resulting elections.
Separately, the unit-test `TestTransport` directly checks the durable vote before
accepting a granted reply. That transport was what caught reply-before-persist
before the simulator's crash schedule was strengthened.

The M2 storage adapter rejects log/snapshot writes. Loaded log/snapshot tails
are supported for voting tests, but replication and full simulated log persistence
will be introduced and tested together in M3.

## Seed campaign and replay

The default campaign covers seeds 0 through 9999, alternating three- and
five-member clusters. These are test inputs, not performance benchmarks.

1. Cold start: connected network. Each seed chooses drop probability from 5-10%,
   duplicate probability from 0-20%, and maximum delay from 1-3 ticks.
   Require a leader within 300 ticks.
2. Crash that leader: retain a connected quorum and the same packet faults.
   Require a different leader in a higher term within another 300 ticks.
3. Restart the old leader and run 180 ticks of faults. Partition into two
   nonempty groups every 30 ticks and heal 15 ticks later. Each phase chooses
   drop probability from 0-20% and duplicate probability from 0-40%. Alternate
   maximum delays of 31-60 ticks with 1-3 ticks. Replies can outlive elections.
   Crash at each granted-reply boundary with a per-seed probability of 20-60%.
   Arm a before/after-sync failure every seven ticks, restart stopped nodes every
   five ticks, and also crash a random node periodically. No liveness bound is
   imposed during these phases.
4. Heal, restart stopped nodes, disable random packet loss/duplication, and use
   one-tick delivery. Disable grant crashes and clear pending sync failures.
   Allow 90 ticks (maximum old delay plus an election timeout) for old traffic, then
   require a unique leader within another 300 ticks.

No bounded liveness is claimed while a majority cannot communicate. Election
timeouts are sampled by integer remainder from 15 through 30 logical
ticks; heartbeats occur every three ticks. The finite seed campaign verifies its
specified bounds, not a universal bound under arbitrary message loss.

The generated summary reports nearest-rank p50/p99 and maximum cold-start and
re-election times, in logical ticks. Re-election timing starts at leader crash
and ends at observing a replacement, including detection, failed rounds, and
retries. The previous campaign's 238-tick maximum was this entire interval,
not one 15-30-tick election timer. Loss and split votes can require multiple
rounds; the earlier evidence did not retain a per-round breakdown for that
maximum. The strengthened campaign measures its own distribution. These are
simulation observations, not real-network failover benchmarks.

The trace digest includes serialized packet contents, scheduling decisions,
event type, logical time, and every member's role and durable hard state.
Single-seed runs also emit the event sequence with a cumulative digest after
each step. Replaying a seed is independent of its offset in a larger campaign.
On failure, the test prints a replay command and reruns that seed with tracing.
The evidence script compares full traces across repeated debug runs and ASan,
and the CI matrix repeats the same replay check on each toolchain.

## Mutation sensitivity gate

`QUORUMKV_BUILD_MUTATION_TESTS` defaults off and requires `BUILD_TESTING`.
Test presets enable it; release disables it. It builds two separate executables:
`quorumkv_mutant_count_stale_votes` and `quorumkv_mutant_reply_before_persist`.
Their private compile definitions affect only their own node implementation.
The normal library and test executable retain both safety rules.

`scripts/verify_mutations.py build/debug --output build/review/mutations.jsonl`
searches up to the full campaign range using only `ElectionFuzz.*`, requires the
historical two-leaders-in-one-term violation, then replays the failing seed twice
and compares complete traces with the search trace. The normal node must pass
the same seed. Timeout failures, crashes, unit-test failures, and a surviving
mutant do not satisfy this gate. Full command output and exit codes are retained
in JSONL and revalidated by the summary producer.

The strengthened campaign first detects stale-vote counting at seed 216 and
reply-before-persist at seed 217. These are deliberate mutation regressions,
not newly discovered defects in the normal election implementation. Local
capture and every CI build configuration run both sensitivity checks.
