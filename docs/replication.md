# Log replication and persistence

The leader durably appends a no-op on election and opaque commands through
`Propose`. Each peer has a speculative next index, an acknowledged match index,
and bounded RPC correlation records. Sending advances next so later batches can
leave before earlier acknowledgements arrive. Only a successful response for a
known current-term RPC advances match. Old or duplicate responses cannot move
match backwards. Rejections use conflict term/index hints to rewind next.
The codec's entry and frame bounds also bound each outgoing batch.

A follower verifies the preceding index and term, replaces only a conflicting
suffix, and saves that replacement before acknowledging it. A matching duplicate
or heartbeat does not truncate a longer suffix. Its commit index is bounded by
the prefix proven by that particular request, not by its unrelated local tail.
The leader advances commit only when a majority acknowledges an entry from the
current term. The newly committed prefix is exposed through the log and commit
index; the simulator applies it in order to an independent entry-history model.
The KV state machine, sessions, and ReadIndex remain M4.

## Historical safety oracle

Every simulator event checks election safety, durable node state, Log Matching,
and application-history safety. The oracle retains history after truncation,
step-down, and crash:

- An `(index, term)` identifies an exact entry and the preceding entry's term.
  Recursively, this requires exact prefix agreement without relying on hashes.
- A committed entry has one canonical value at its index. Each node's previously
  committed prefix must remain in its durable log after any later write or crash.
- Each newly elected leader must contain every entry already known committed.
  An isolated older leader is not treated as a newly elected leader on each tick.
- Within one node lifetime, commit cannot move backwards. Restart resets the
  volatile application cursor, then replays the same canonical committed prefix.

Unchanged durable images reuse a previously validated result, keyed by the
storage revision; the event still checks the node against that image and counts
both safety checks. Any durable write invalidates the cached result. Trace hashes
identify executions, but the safety comparisons use exact entries and bytes.

The random campaign runs on both three- and five-member clusters, varies packet
drop/duplicate/delay rates and batching, proposes writes, partitions and heals
links, injects sync failures, and crashes/restarts nodes. Some delivery delays
exceed election timeouts. Every seed ends with all nodes crashing and recovering
from durable storage, then requires equal committed logs after the network heals.
Arbitrary asynchronous phases have no liveness deadline.

Additional directed phases use the same simulator and only deliver real queued
RPCs. The Figure 8 schedule demonstrates both an uncommitted old-term majority
being overwritten and a current-term majority protecting its prefix. Another
schedule crashes a follower immediately after its acknowledgement and then
crashes the leader, forcing recovery to depend on the acknowledged durable copy.
These phases also appear throughout the seeded campaign.

The Figure 8 schedules follow section 5.4.2 of the
[extended Raft paper](https://raft.github.io/raft.pdf).

## Storage boundaries

`MemoryStorage` builds an entire pending image containing the term, vote, and log.
Failure before sync leaves the previous durable image; failure after sync keeps
the complete new image. Crashes discard only pending state. The transport can
also unwind a node directly at a send boundary, preventing later persistence
instructions from executing.

`FileStorage` implements the same batch interface with complete checksummed images.
It writes and syncs a staging file, atomically replaces the live image in the same
directory, and completes the platform's publication synchronization. Recovery
reads only the live image and ignores an abandoned staging file. Malformed,
truncated, and checksum-invalid images fail closed. A failed publication stops
the adapter until it is reopened, because the durable outcome may be ambiguous.

Windows uses `FlushFileBuffers` and same-volume `MoveFileExW` with replacement
and write-through flags. POSIX uses `fsync`, `rename`, then `fsync` of the containing
directory. The platform contracts are documented by
[Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
and the [Linux fsync manual](https://man7.org/linux/man-pages/man2/fsync.2.html).
The directory must already exist, and one instance owns it. Storage is bounded
to a 64 MiB image and rewrites the full image per batch; this is a correctness
implementation, not a measured storage-performance design. Snapshot publication
and compaction remain M5. Hardware power-loss behavior is not simulated by the
file tests; they exercise publication boundaries and actual sync calls.

## Mutation sensitivity and diagnostics

Test presets generate separate unsafe node libraries from explicit transformations
of the production source. Generation fails if an anchor changes unexpectedly.
Production sources contain no mutation preprocessor branches. The variants cover
stale-term votes, vote replies before persistence, self-votes after sending,
old-term commits, append replies before persistence, double voting, and a
two-of-five election majority.

Each sensitivity check requires a simulator invariant failure, two identical
failing replays, and a passing unmodified-node control. The debug survey runs
every variant over the full seed range and reports detection counts, including
seeds that do not detect the defect. Timeouts, process crashes, and unexpected
exceptions do not count as successful detections.

Candidate crashes check the durable announced term and self-vote at the
RequestVote send boundary. This is an explicit persistence-before-effect
invariant, not a claim that every such crash produces two leaders.
The unit-test transport independently checks vote durability at send time.

CTest discovers individual tests and sets a timeout on each. The launcher also
detects a process that prints the final passing summary but fails to exit:
it captures an all-thread Windows minidump or a POSIX gdb backtrace, then fails
and kills the process. Dumps stay under ignored `build/stacks/` and CI retains
them as artifacts. Long seed campaigns have a separate finite deadline.
