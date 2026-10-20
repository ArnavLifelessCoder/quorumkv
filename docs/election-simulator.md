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
crash, restart, or fault/partition change. A node call and its persistence/enqueue
effects are indivisible; there is no interleaved callback. The historical safety
oracle runs after every such step, including storage-failure crashes.

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

The M2 storage adapter rejects log/snapshot writes. Loaded log/snapshot tails
are supported for voting tests, but replication and full simulated log persistence
will be introduced and tested together in M3.

## Seed campaign and replay

The default campaign covers seeds 0 through 9999, alternating three- and
five-member clusters. These are test inputs, not performance benchmarks.

1. Cold start: connected network, 10% drop probability, 10% duplicate probability,
   and seeded delays of one through three ticks. Require a leader within 300 ticks.
2. Crash that leader: retain a connected quorum and the same packet faults.
   Require a different leader in a higher term within another 300 ticks.
3. Restart the old leader and run 180 ticks of faults. Partition into two
   nonempty groups every 30 ticks and heal 15 ticks later. Add repeated hard
   crashes, restarts, and failures before/after hard-state sync.
4. Heal, restart stopped nodes, disable random packet loss/duplication, and use
   one-tick delivery. Allow 40 ticks for old traffic and isolated leaders, then
   require a unique leader within another 300 ticks.

No bounded liveness is claimed while a majority cannot communicate. Election
timeouts are sampled by integer remainder from 15 through 30 logical
ticks; heartbeats occur every three ticks. The finite seed campaign verifies its
specified bounds, not a universal bound under arbitrary message loss.

The trace digest includes serialized packet contents, scheduling decisions,
event type, logical time, and every member's role and durable hard state.
Single-seed runs also emit the event sequence with a cumulative digest after
each step. Replaying a seed is independent of its offset in a larger campaign.
On failure, the test prints a replay command and reruns that seed with tracing.
The evidence script compares full traces across repeated debug runs and ASan,
and the CI matrix repeats the same replay check on each toolchain.
