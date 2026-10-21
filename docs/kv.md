# KV state machine, client sessions, and ReadIndex (M4)

M4 puts a replicated key-value store on top of the M3 log and proves two things in the
simulator: retried client requests take effect exactly once, and reads never return
stale data. Real networking is still M6; simulated clients call the nodes directly.

## Commands and their semantics

`src/kv/store.cpp` implements `StateMachine` with an ordered `std::map`. Commands and
responses use a hand-written, version-tagged, big-endian encoding (`kv::EncodeRequest`
and friends). Decoders reject truncation, trailing bytes, unknown tags, noncanonical
booleans, and blobs over `kMaxBlobBytes`. A command that does not decode is still
"applied": it has no effect and returns `Invalid`, so every replica stays in lockstep.

| Operation | Effect | Response |
| --- | --- | --- |
| `Put(key, value)` | Sets the key | `WriteOk` |
| `Get(key)` | None | `Value{found, value}` |
| `Append(key, suffix)` | Appends, creating the key if missing | `WriteOk` |
| `CAS(key, expected, new)` | Sets `new` only if the key exists and equals `expected` | `CasResult{exchanged, found, previous}` |

These choices were left open by the client protocol plan and are fixed here:

- **CAS on a missing key never exchanges.** An empty `expected` matches an existing empty
  value, not absence. The response reports the value before the operation.
- **Sequence gaps are accepted.** A client may abandon a request it never saw answered.
  A sequence above the session's latest executes.
- **A retry of the latest sequence returns the cached response**, even for `Get` through
  the log, where the key may have changed since. It is the answer the original would have had.
- **An older sequence returns `StaleSequence` and has no effect.** Clients only retry their
  latest request, so the workload treats this response as a bug.
- **ReadIndex reads carry no session.** A retried read simply reads again; reads have no
  effect, so this is safe and still linearizable.

## Sessions and exactly-once

Each client has a stable `ClientId` (two 64-bit integers, never allocated by RaftNode) and
an increasing sequence number. The store keeps, per client, the latest applied sequence
and its encoded response. Because sessions live in the replicated state machine, every
replica deduplicates identically, and `Snapshot()`/`Restore()` carry the session table
(tested in `KvStore.SnapshotRestoresDataSessionsAndDigestExactly`). M5 will exercise this
through InstallSnapshot.

## Applying entries

RaftNode applies entries in log order once `commitIndex` passes them and reports each
command's `{index, term, response}`. `lastApplied` is volatile: after a restart, a fresh
state machine is rebuilt by reapplying the retained committed prefix. A client that
submitted at `(index, term)` knows its command was replaced if the entry applied at that
index has a different term.

## ReadIndex

A leader serves `Read(id, query)` only after all of these hold:

1. It has committed an entry from its current term. The no-op appended at election
   time does this. Until then the leader does not know the true commit point.
2. It records `readIndex = commitIndex` and a new, increasing read context, and sends a
   heartbeat carrying that context.
3. A majority, counting itself, replies in the current term to a heartbeat whose
   context is at least the read's. Replies to heartbeats sent before the read was
   registered do not count (`ReadIndexTest.NewLeaderWaitsForItsNoOpAndAFreshMajorityHeartbeat`).
4. `lastApplied >= readIndex`. The read then runs against the local state machine.

If the node stops being leader first, every pending read fails and the client retries.
No time-based lease is used, so correctness never depends on clock bounds.

### Why reading straight from leader memory is not linearizable

A node that believes it is leader may already have been replaced. Suppose leader A is
partitioned from the other two nodes. They elect B, and a client writes `x = 2` through B,
which commits and acknowledges it. A has not heard about any of this and still
answers reads with `x = 1`. A read that starts after the write was acknowledged returns the
old value, which no linearizable order allows. The heartbeat round in step 3 is what rules
this out: A cannot collect a majority of current-term replies while a newer leader exists,
because that majority overlaps the one that elected B.

Step 1 matters for the same reason in a different case. A newly elected leader may hold
entries committed by its predecessor that it does not yet know are committed. Its own
`commitIndex` can lag behind what earlier clients were told succeeded, until it commits
something in its own term.

`ElectionConfig::unsafe_local_reads` deliberately serves reads from local state on any
node. It exists only so the checker can prove it catches this class of bug.

## Simulated clients and the version check

`src/sim/kv_workload.cpp` runs 3 to 5 clients against a 3- or 5-node simulated cluster.
For 300 ticks it applies the M3 nemesis: partitions, leader crashes, restarts, sync failures,
power loss after votes and append acknowledgements, long delays, drops, and duplicates. Clients:

- pick Append (unique `cNsM;` tokens on keys `a0`/`a1`), Put or CAS (unique values on
  `r0`/`r1`), and Get (90% through ReadIndex, 10% through the log);
- follow `NotLeader` leader hints, and otherwise rotate through nodes;
- time out after 30 ticks or when the node they wait on restarts, and resend the **same**
  sequence to another node. This is what produces real duplicate submissions.

After healing, every outstanding operation must finish within 1500 ticks. Final values are
then read through the normal ReadIndex path. `CheckHistory` (`src/sim/kv_history.cpp`)
orders events by a single global counter and checks:

| Property | Keys |
| --- | --- |
| Exactly-once: each token appears at most once in the final value | append |
| Durability: every acknowledged append is in the final value | append |
| Real-time order: an append acknowledged before another was invoked precedes it | append |
| Reads return a prefix of the final value (only committed states are visible) | append |
| Stale read: a read contains every append acknowledged before it was invoked | append |
| Read from the future: every token read was appended before the read completed | append |
| A missing read after an acknowledged write is stale; values must have a writer | register |

The append-key checks are the per-key version check from the spec: a value's token count
is its version. The simulator also checks, at every step, that all replicas produce byte-identical
responses at each index, on top of the M3 Raft invariants. Full register linearizability (CAS
order) is the job of the M7 WGL checker.

## The checker finds real bugs

`KvFuzz.CheckerFlagsUnsafeLocalReads` and `KvFuzz.CheckerFlagsMissingSessionDedupe` run
deliberately broken modes until the checker fails, and fail themselves if it never does. The
first flags reads served from local state on any node. The second uses a store that forgets
sessions, so every retry executes again. The seeds that catch each one are recorded in `results/m4/`.
