# Real networking (M6)

## One node per process, one thread per node

`quorumkv_server` runs a single event loop (`src/server/server.cpp`). The loop polls every
socket (`poll` on POSIX, `WSAPoll` on Windows), feeds complete frames to the node, and calls
`RaftNode::Tick()` every `--tick-ms` (10 ms by default, giving 150 to 300 ms election timeouts
and 30 ms heartbeats). RaftNode still sees only the injected interfaces. The adapters are:

- `TickClock`: logical ticks from `std::chrono::steady_clock`, with no wall-clock epoch;
- `ProcessRandom`: SplitMix64 seeded once per process, never a global generator;
- `FileStorage`: the M3/M5 checksummed image with fsync and atomic replace;
- a transport that queues encoded frames on outbound peer connections.

Because a server owns all of its state, several can run in one process on separate threads.
`tests/server_test.cpp` does exactly that over real localhost TCP, and the Linux TSan job
checks it.

## Links

Peers and clients use separate listeners. Each node dials every other node, and only sends
on the connections it dialed; it receives on connections others dialed to it. A message
queued while its link is down is dropped, which Raft already tolerates as network loss.
Reconnects are retried every 100 ms. Outbound queues are capped at 8 MiB per peer.

## Client protocol

Client frames share the peer framing (32-bit length prefix, version byte) with tags 7 and 8.
A malformed frame closes that connection and never affects the node.

| Request kind | Payload | Served by |
| --- | --- | --- |
| Command (1) | `kv::EncodeRequest` | Propose, answered when the entry applies |
| Read (2) | `kv::EncodeRead` | ReadIndex |
| Status (3) | empty | role, term, leader, commit, applied, snapshot index |

| Response status | Meaning |
| --- | --- |
| Ok (0) | applied; payload is the `kv::Response` |
| NotLeader (1) | never proposed here; optional `leader_hint` |
| Retry (2) | accepted, but the outcome is unknown: another leader's entry replaced it, a snapshot covered it, or a read lost leadership |

A command is answered when its index applies. It succeeds only if the applied entry has the
term it was proposed in; a different term means another leader's entry won that index.

## Client library and outcomes

`quorumkv::Client` sends one request at a time, follows leader hints, rotates on connection
failures, and backs off when every node refuses. Writes keep one session sequence across all
retries, so the server deduplicates them. Each operation ends in one of three outcomes:

- **ok**: applied exactly once, with its result;
- **fail**: no attempt ever reached a node that could have applied it;
- **unknown** (`info` in histories): some attempt may have been applied, for example after a
  timeout or a `Retry` response.

Reads have no effect, so a read that never succeeds is reported as `fail`.

## Process faults on Windows

The spec names SIGKILL, SIGSTOP and SIGCONT. The harness uses psutil, which maps them to
`TerminateProcess` and thread suspend and resume on Windows. A suspended process stops
running, keeps its sockets open, and resumes where it stopped, which is what a long GC pause
looks like to its peers.
