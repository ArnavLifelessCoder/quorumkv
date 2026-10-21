# M4 review packet

Status: implementation complete on branch `m4`. Source commit, evidence capture
(`scripts/capture_m4.ps1`), CI, and owner approval are pending. M5 has not started.

## Built

- `src/kv/store.cpp`: the `std::map` state machine behind `StateMachine`, with Put, Get,
  Append, and CAS, a per-client session table, and Snapshot/Restore that carries sessions.
  Hand-written versioned codec for requests, reads, responses, and snapshots.
- `RaftNode` applies committed entries in log order to an optional state machine and reports
  `{index, term, response}`. It serves `Read` through ReadIndex: a current-term commit, then a
  majority reply to a heartbeat carrying the read's context, then `lastApplied >= readIndex`.
  Pending reads fail when leadership is lost. Without a state machine and without reads the node
  sends byte-identical messages to M3, so the election and replication digests are unchanged.
- The simulator rebuilds a fresh state machine on every boot from the retained log, exposes
  client `Submit`/`Read` entry points that fail on a mid-call crash, and checks that every
  replica produces identical responses at each committed index.
- `src/sim/kv_workload.cpp`: 3 to 5 clients under the M3 nemesis, with timeouts that resend
  the same sequence to another node. `src/sim/kv_history.cpp`: the per-key version checker.
- Negative controls: `unsafe_local_reads` and a test-only store without session dedupe.
- M3 follow-up: CMake minimum restored to 3.20; the exit-hang watchdog is used on 3.29+.

See [the KV design](kv.md) for semantics, the ReadIndex argument, and the checker properties.

## Acceptance gates

- [ ] Duplicate client requests (simulated retries) apply exactly once:
  `KvFuzz.ExactlyOnceAndFreshReadsAcrossSeededFaults` over 10,000 seeds, plus
  `KvStore.RetriedSequenceAppliesOnceAndReturnsTheOriginalResult`.
- [ ] Reads never return stale data in sim histories, by the per-key version check, over the
  same 10,000 seeds.
- [ ] The checker flags both deliberately broken modes
  (`KvFuzz.CheckerFlagsUnsafeLocalReads`, `KvFuzz.CheckerFlagsMissingSessionDedupe`).
- [ ] ReadIndex unit tests: stale heartbeat replies do not confirm a read, reads wait for the
  new leader's no-op and for apply, and step-down fails pending reads (`ReadIndexTest.*`).
- [ ] Session dedupe survives Snapshot/Restore (`KvStore.SnapshotRestoresDataSessionsAndDigestExactly`).
- [ ] M1 to M3 campaigns unchanged: same election and replication digests as `results/m3/`.
- [ ] Debug and ASan green locally with zero warnings; clang-tidy, format, ruff, pytest pass.
- [ ] Clean-source capture published to `results/m4/` and all final-commit CI jobs green.
- [ ] Owner approval.

Every number for this milestone comes from `results/m4/summary.json` once captured.

## Files changed since M3

- Core: `include/quorumkv/raft_node.hpp`, `src/raft/node.cpp`.
- KV: `include/quorumkv/kv.hpp`, `src/kv/store.cpp`.
- Simulator: `include/quorumkv/sim_cluster.hpp`, `src/sim/cluster.cpp`,
  `include/quorumkv/kv_history.hpp`, `src/sim/kv_history.cpp`,
  `include/quorumkv/kv_workload.hpp`, `src/sim/kv_workload.cpp`.
- Tests: `tests/kv_store_test.cpp`, `tests/read_index_test.cpp`, `tests/kv_history_test.cpp`,
  `tests/kv_fuzz_test.cpp`, `tests/python/test_m4_summary.py`.
- Evidence tooling: `scripts/capture_m4.ps1`, `scripts/summarize_m4.py`,
  `scripts/verify_kv_replay.py`.
- Build, CI, and docs: `CMakeLists.txt`, `.github/workflows/ci.yml` (KV replay check),
  `README.md`, `docs/kv.md`, `docs/m4-review.md`, `docs/client-protocol-plan.md`.
