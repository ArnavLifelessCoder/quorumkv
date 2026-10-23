# M5 review packet

Status: complete on branch `m5`. Evidence comes from the exact-commit CI run for source
`2af72e6` ([run 39947796243](https://github.com/ArnavLifelessCoder/quorumkv/actions/runs/39947796243)),
recorded by `scripts/record_ci_evidence.py` in `results/m5/` (summary in `ci.json`, full job
logs beside it). As for M4, CI is the evidence of record because local multi-hour captures were
cut off when the machine slept. `scripts/capture_m5.ps1` produces the same lines locally.

## Built

- Snapshot publication in `ApplyStorageBatch`, following Figure 13 receiver rules 6 and 7.
- `FileStorage` format `QKVLOG02` with a checksummed snapshot section; `QKVLOG01` still loads.
- Compaction on `Tick` past `snapshot_threshold` applied entries.
- Chunked InstallSnapshot with in-order assembly, persist-before-reply, restore, and leader
  progress tracking.
- Restore on boot from the durable snapshot, followed by replay of the retained suffix.
- Oracle checks: snapshot points match the committed history, snapshots of one index are
  identical, restored machines equal their snapshots, and snapshot plus replay equals replay.
- A KV campaign with snapshots on in most seeds, and a negative control whose snapshots drop
  sessions.

See [the snapshot design](snapshots.md).

## Acceptance gates

All seven CI jobs passed. Each of the five test jobs ran 104 tests, and every campaign summary
line is identical across all five.

- [x] A lagging follower catches up through a snapshot:
  `SimSnapshot.LaggingFollowerCatchesUpThroughChunkedInstallSnapshot` passes, and the
  10,000-seed KV campaign completed 141 InstallSnapshot transfers in 124,011 chunks.
- [x] Restart from snapshot plus remaining log gives identical state:
  `SimSnapshot.RestartFromSnapshotPlusLogReproducesIdenticalState` passes, and the campaign ran
  60,242 restore checks with no mismatch across 34,870 snapshots.
- [x] Log size stays bounded under a long-running workload: the `[kv-longrun]` run of 1,500
  ticks applied 7,870 commands and never retained more than 20 entries (threshold 16).
- [x] Session dedupe survives snapshots: the campaign completed all 175,018 operations with
  snapshots on and no exactly-once or stale-read failure, and the sessionless-snapshot control
  fails at seed 1 ("replayed state differs from a snapshot of the same index").
- [x] Debug and ASan green on Windows and Linux, TSan on Linux, clang-tidy, format, ruff and
  pytest green in the same run.
- [x] M1 to M4 digests: the election and replication digests equal `results/m3/`.

Every number comes from `results/m5/ci.json`.
