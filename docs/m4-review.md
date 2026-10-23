# M4 review packet

Status: complete on branch `m4`. Evidence comes from the exact-commit CI run for source
`ba0b916` ([run 39637924544](https://github.com/ArnavLifelessCoder/quorumkv/actions/runs/39637924544)),
recorded by `scripts/record_ci_evidence.py` in `results/m4/` (summary in `ci.json`, full job
logs beside it). Local multi-hour ASan captures on the development machine were repeatedly
cut off when the machine slept, so the evidence of record is the CI run, which builds and tests
debug and ASan on Windows and Linux plus TSan on Linux.

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

All seven CI jobs passed. Each of the five test jobs ran 94 tests, and every campaign summary
line is identical across all five, which also shows the simulator is deterministic across
compilers and platforms.

- [x] Duplicate client requests apply exactly once: the 10,000-seed KV campaign completed all
  181,933 operations, with 66,659 retries of an unknown outcome and 16,402 reads through the
  log. Every history passed the exactly-once check, and every step passed the safety checks
  (31,344,275 steps).
- [x] Reads never return stale data: the same campaign served 68,409 ReadIndex reads, and the
  per-key version check found no stale or uncommitted read.
- [x] The checker flags both broken modes: unsafe local reads at seed 14 ("Stale read") and a
  store without session dedupe at seed 0 ("Exactly-once").
- [x] ReadIndex unit tests (`ReadIndexTest.*`) and session snapshot tests pass.
- [x] M1 to M3 campaigns unchanged: the election and replication digests equal `results/m3/`.
- [x] Debug and ASan green on Windows and Linux, TSan on Linux, clang-tidy and lint green.
- [x] Owner approval: given by the owner ("commit, merge, do everything").

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
