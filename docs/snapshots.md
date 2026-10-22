# Snapshots and InstallSnapshot (M5)

M5 bounds the log. A node replaces its applied prefix with a snapshot of the state machine,
and a leader brings back a follower that is too far behind by sending that snapshot instead of
entries it no longer has.

## Storage

A `StorageBatch` can now carry a snapshot. `ApplyStorageBatch` applies the batch in the
documented order: truncate, append, publish the snapshot, update hard state. For the snapshot
step it follows the receiver rules in the extended paper's Figure 13:

- If the log holds an entry at the snapshot's last included index with the same term, the
  entries after it are kept.
- Otherwise the snapshot supersedes the whole log, and every entry is discarded.
- A snapshot may never move the compacted prefix backwards, and its term may not be zero.

`FileStorage` writes images in format `QKVLOG02`, which adds a snapshot section (index, term,
length, bytes) after the entries, inside the checksum. Format `QKVLOG01` images, which never
held a snapshot, remain readable. The publication protocol is unchanged: write a temporary file,
fsync it, then atomically replace the image.

## Compaction

`ElectionConfig::snapshot_threshold` turns compaction on. During `Tick`, once at least that many
applied entries are retained, the node takes `StateMachine::Snapshot()` at `lastApplied` and
persists it. Compaction happens only on ticks, never in the same step that commits or applies
an entry. The simulator's oracle therefore always observes a commit before the entries
disappear into a snapshot.

Only applied entries are compacted. Uncommitted entries stay in the log, so a long partition
can still grow the uncommitted suffix; the bound applies to committed history.

## InstallSnapshot

A leader whose `next` index for a follower falls inside its snapshot sends the snapshot in
chunks of `snapshot_chunk_bytes`. One chunk is outstanding per follower. Each heartbeat
resends it with a new RPC id, and only the reply to the latest RPC is acted on. The follower:

1. rejects a stale term, and otherwise treats the sender as leader and resets its election timer;
2. answers "complete" if the snapshot covers nothing beyond its commit index;
3. accepts chunks strictly in order, replying with the offset it expects next, and restarts
   on offset zero or on a different snapshot;
4. on the last chunk, persists the snapshot, then restores the state machine, then sets
   `commitIndex` and `lastApplied` to the snapshot index, and only then replies.

On a successful final reply the leader sets the follower's `match` to the snapshot index and
resumes with AppendEntries.

## Restart and sessions

On boot a node restores its state machine from the durable snapshot. `commitIndex` and
`lastApplied` start at the snapshot index, and the retained suffix is reapplied once the
commit point is learned. The KV snapshot contains the session table, so a retry that arrives
after a restore or an InstallSnapshot is still recognized.

## What the simulator checks

On top of the M3 and M4 invariants, at every step:

- A snapshot may cover only entries some node was observed to commit, and its last included
  term must match the committed history at that index.
- Snapshots taken at the same index on different nodes must be byte-identical.
- After every boot, the restored state machine must encode exactly to its durable snapshot.
- Whenever a node's `lastApplied` reaches an index for which a snapshot exists, its state
  machine must encode to that snapshot. This compares snapshot-plus-replay against replay
  from the beginning.

The KV campaign runs with snapshots enabled in four of every five seeds, a threshold of 4 to 16
entries, and 8 to 68 byte chunks, so most transfers take several messages and many are
interrupted by faults. `KvFuzz.CheckerFlagsSnapshotsThatDropSessions` runs a store whose
snapshots lose the session table and requires the checks above to flag it.

The M5 acceptance tests are in `tests/snapshot_test.cpp`:

- `SimSnapshot.LaggingFollowerCatchesUpThroughChunkedInstallSnapshot`
- `SimSnapshot.RestartFromSnapshotPlusLogReproducesIdenticalState`
- `SimSnapshot.LogStaysBoundedUnderALongRunningWorkload`
