# Client codec plan before M4

This is a design plan, not an implemented protocol extension. The M1 codec still
accepts only peer tags 1 through 6. Implement and test the client extension at the
start of M4, before using the codec for client sessions or ReadIndex. Real TCP
connection handling remains M6 work.

## Framing and separation

Keep the bounded length/version/tag prefix. Reserve tag 7 for ClientRequest and
tag 8 for ClientResponse. Do not reuse a peer NodeId as a client identity or
pretend that a client is a Raft peer. Introduce a top-level wire-message variant
that dispatches to the peer envelope or the client schema after reading the tag.
Peer tags retain their existing golden-frame bytes; client messages do not carry
the peer envelope's sender, recipient, and term fields.

An old decoder must continue rejecting unknown tags. No rolling-upgrade
compatibility is promised. Both codecs retain exact-frame consumption, version
checks, canonical booleans, and allocation limits. The RPC server dispatches
client requests to the client/session layer, never directly to RaftNode::Step.

## Proposed request and response types

ClientRequest carries an opaque transport request ID, a ClientId, a sequence
number, and an operation variant. ClientId is an opaque pair of unsigned 64-bit
integers; sequence and request ID are unsigned 64-bit integers. Client IDs remain
stable across retries and must not be allocated by RaftNode using global randomness.

| Operation | Fields |
| --- | --- |
| Put | key blob, value blob |
| Get | key blob |
| Append | key blob, suffix blob |
| CAS | key blob, expected-value blob, replacement-value blob |

ClientResponse echoes request ID, client ID, and sequence number, then carries an
explicit result variant rather than ambiguous flags:

- WriteOk for an acknowledged write.
- Value with an explicit found boolean and a value blob when present, so missing
  and an existing empty value are distinct.
- CASResult with an exchanged boolean and the defined CAS return payload.
- `NotLeader{leader_hint}` with an optional NodeId. Absence is a boolean, not a
  special node number.
- InvalidRequest or StaleSequence for explicitly defined client/session errors.

Client operation/result tags and all integer widths will be documented alongside
golden frames when implemented. Bounds apply to each blob and to the whole frame;
an operation containing several individually legal blobs may still exceed the
frame budget and must be rejected before admission.

## Admission, retries, and semantics to settle at M4

Return NotLeader before admitting a request on a follower. A leader hint is only
a routing suggestion and is not proof that the hinted node remains leader.
Clients retry the same logical operation with the same client ID and sequence.
An RPC ID may change per attempt and must not be used for deduplication.

A timeout or leadership loss after admission is an unknown outcome, not proof
that the operation did not apply. A write is acknowledged only after committed
application. Session results must survive snapshots. Get must complete ReadIndex
and wait for application through its read index before returning a value.

Before implementing the M4 state machine, resolve and document missing-key CAS
semantics, the precise CAS response payload, treatment of sequence gaps/old
retries, and whether read retries cache responses or repeat a fresh linearizable
read. These choices affect observable behavior and are not fixed by the current
spec; do not silently choose them while implementing the state machine.

## Required codec tests before M4 integration

Add golden frames and seeded round trips for every operation and result, including
both present and absent leader hints. Extend the independent reference encoder.
Exercise empty/binary keys and values, maximum IDs/sequences, every truncated
prefix, unknown operation/result tags, noncanonical presence flags, invalid
operation-specific payloads, and combined frame-budget overflow.

Verify dispatch rejects client messages on the peer path and peer messages on the
client path. Keep existing peer golden frames and the current regression corpus
unchanged. Session retry tests and ReadIndex correctness belong to the M4
simulator suite, not merely to codec round trips.
