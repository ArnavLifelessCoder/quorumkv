# Peer wire protocol, version 1

M1 defines peer messages and their byte encoding. It does not implement elections,
log replication, storage adapters, the KV engine, or TCP connections.

Every frame starts with a four-byte unsigned payload length, excluding that
prefix. All integers are big-endian. The payload starts with one version byte
(`1`), one tag byte, then four unsigned 64-bit integers: sender node ID, recipient
node ID, current term, and RPC ID. Replies echo the request RPC ID. The core owns
RPC ID allocation; it does not depend on a global random source.

Booleans occupy one byte and must be `0` or `1`. A blob is an unsigned 32-bit byte
length followed by exactly that many bytes, including any embedded zeros.
Optional terms are a presence boolean followed by a 64-bit term only when present.
Zero is a valid represented term and is distinct from an absent optional value.

| Tag | Body | Fields in order |
| --- | --- | --- |
| 1 | RequestVote | last log index, last log term |
| 2 | VoteResponse | granted boolean |
| 3 | AppendEntries | previous log index, previous log term, leader commit index, read context, entry count, entries |
| 4 | AppendResponse | success boolean, match index, optional conflict term, conflict index, read context |
| 5 | InstallSnapshot | last included index, last included term, chunk offset, done boolean, chunk blob |
| 6 | SnapshotResponse | success boolean, next expected chunk offset |

Unless otherwise stated, table fields are unsigned 64-bit integers. Entry count is
unsigned 32-bit. Each entry is an index, term, one-byte kind (`0` for no-op, `1` for
command), then a command blob. The command bytes are opaque to this codec.
Read context is an opaque identifier echoed by heartbeat responses; zero means
there is no read request associated with the message. The later ReadIndex
implementation must match the context, term, RPC, and sender before counting replies.

Frames are capped at 4 MiB including the prefix, blobs at 1 MiB, and entry vectors
at 4096 entries. A sender must batch entries and split snapshots to meet these
limits. Both encoder and decoder enforce the limits. The decoder checks lengths
and the minimum bytes needed for an entry vector before allocating. It requires
one complete frame and rejects incomplete frames, unsupported versions, unknown
tags/kinds, invalid booleans, and trailing bytes. Stream buffering belongs to the
future transport; it must bound the prefix before allocating a receive buffer.

The codec enforces structural validity. Raft rules such as contiguous indices,
valid peers, and consistency with a prior term are enforced by the Raft core.
Client command and response encoding will be defined with the KV layer.

## Interface contracts

`Transport::Send` queues an owned copy without synchronously calling the sender
back. Its destination argument must equal the message recipient.

`Clock::Now` returns monotonic logical ticks. A simulator controls ticks explicitly;
a real adapter will translate a monotonic clock. Election logic does not know the
wall-clock epoch or sleep directly.

`Random::Next` supplies unsigned 64-bit values. The simulator must own and seed its
random streams. The M1 test generator uses specified SplitMix64 arithmetic, avoiding
implementation-dependent distribution behavior.

`Storage::Load` returns durable hard state, entries, and an optional snapshot.
`Persist` is an atomic, synchronous durable batch. Its conceptual order is:

1. Remove entries at or after `truncate_from`, when supplied.
2. Append the supplied entries.
3. Publish the supplied complete snapshot and discard entries through its last
   included index, when supplied.
4. Replace hard state, when supplied.

Absent optional fields mean no change. All effects commit together. Return means
the batch is durable; on failure the adapter throws and the node must stop rather
than acknowledge the RPC. A crash during a batch may expose the previous or the
new complete durable state, never a partial batch. The memory and file adapters
implement these boundaries as described in [replication and persistence](replication.md). Snapshot transport
chunks must be assembled before publishing a complete storage snapshot in M5.
Filesystem synchronization and atomic rename implementations live behind
Windows and POSIX adapters in `src/platform/`.

`StateMachine` operates on opaque command and response bytes. `Apply` accepts only
committed commands. `Read` requires the caller to complete ReadIndex first.
`Snapshot` and `Restore` include data and client session responses together. M4
will supply the `std::map` implementation; the LSM engine is not a dependency.
