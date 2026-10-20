# M3 review packet

Owner approval is pending. M4 has not started. M2 was approved and pushed to
`main`, which is now the repository default branch; M3 stays on `m3`.

## Built

Validated log views and atomic suffix batches; durable leader no-ops and command
proposals; batched and pipelined AppendEntries; fast conflict backup; correlated,
reorder-safe acknowledgements; current-term-only majority commits; memory and
filesystem persistence; historical simulator safety checks and directed Figure 8
and acknowledgement-crash schedules.

The [design](replication.md) describes the oracle, crash model, storage limits,
mutation isolation, and watchdog. The KV state machine is still scheduled for M4,
behind the existing StateMachine interface, with `std::map` as its initial backend.

## Changed files

- Core: `include/quorumkv/log.hpp`, `raft_node.hpp`, `file_storage.hpp`, and
  `src/raft/{log,node,file_storage}.cpp`.
- Simulator: `include/quorumkv/{sim_support,sim_cluster,replication_safety}.hpp`
  and `src/sim/{support,cluster,replication_safety}.cpp`.
- Platform: `src/platform/file.hpp`, Windows/POSIX sync and replacement adapters,
  and native stack-capture helpers.
- Tests: log, replication, storage, historical oracle, Figure 8 and crash
  schedules, seeded replication campaign, mutation verification, evidence
  validation, and watchdog tests.
- Build and evidence: CMake targets/discovery/timeouts, CI, mutation generation,
  M3 capture/replay/summary/CI scripts, README, and milestone design/review notes.

## Full output and provenance

The [generated summary](../results/m3/summary.json) reports the tested clean source
SHA, test counts, seed ranges, fault counts, safety-check counts, recovery bounds,
replay fingerprints, and per-mutation detection counts. Evidence is committed
separately from that clean source; the results-only commit changes no source.

- [Debug configure](../results/m3/debug-configure.txt),
  [build](../results/m3/debug-build.txt), [full tests](../results/m3/debug-tests.txt),
  and [tool/source metadata](../results/m3/debug-tools.json).
- [ASan configure](../results/m3/asan-configure.txt),
  [build](../results/m3/asan-build.txt), [full tests](../results/m3/asan-tests.txt),
  and [tool/source metadata](../results/m3/asan-tools.json).
- [Debug mutations and full surveys](../results/m3/debug-mutations.jsonl),
  [ASan mutation replays](../results/m3/asan-mutations.jsonl).
- [Replication replay](../results/m3/replication-replay.txt),
  [election replay](../results/m3/sim-replay.txt), [codec replay](../results/m3/replay.txt).
- [clang-tidy](../results/m3/clang-tidy.txt), [Python tests](../results/m3/python-tests.txt),
  and [format/lint](../results/m3/lint.txt).
- [CI run and all job results](../results/m3/ci.json),
  [complete Ubuntu TSan job output](../results/m3/ci-tsan.txt).

`scripts/capture_m3.ps1` refuses a dirty source tree and publishes only when every
local check, replay proof, mutation survey, and exact-source CI gate passes.
`scripts/summarize_m3.py` generates the summary from logs; `scripts/fetch_ci.py`
produces the CI record and TSan output.

## Acceptance criteria for review

The corresponding tests and evidence gates are:

| Criterion | Verification |
|---|---|
| Committed entries survive leader changes, crashes, and partitions without reorder | `ReplicationFuzz.CommittedHistorySurvivesSeededFaults`, directed acknowledgement-crash test, durable-prefix oracle |
| State Machine Safety and Log Matching after every step | Historical oracle; step/check counters validated by the summary producer; negative oracle unit tests |
| Figure 8 | Both directed branches, seeded Figure 8 phases, old-term-commit mutation |
| Durable versus pending log entries | Before-/after-sync memory tests; file publication/reopen tests; append-before-persist mutation |
| Failing seeds print and replay identically | Mutation search, two failing replays, normal controls, and debug/ASan trace comparison |
| Injected dependencies only in RaftNode | Core uses only Transport, Clock, Storage, and Random for external effects |
| Debug, ASan, lint, tidy, and CI green | Full local logs, exact-source CI record, and Ubuntu TSan output linked above |

The results do not establish reviewer acceptance by themselves. Approval remains
an explicit owner decision.
