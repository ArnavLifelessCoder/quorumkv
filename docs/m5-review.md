# M5 review packet

Status: implementation complete on branch `m5`. Evidence capture (`scripts/capture_m5.ps1`)
and CI for the final commit are recorded in `results/m5/`.

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

- [ ] A lagging follower catches up through a snapshot:
  `SimSnapshot.LaggingFollowerCatchesUpThroughChunkedInstallSnapshot`, plus InstallSnapshot
  completions across the 10,000-seed KV campaign.
- [ ] Restart from snapshot plus remaining log gives identical state:
  `SimSnapshot.RestartFromSnapshotPlusLogReproducesIdenticalState`, and the restore and replay
  checks on every step of the campaign.
- [ ] Log size stays bounded under a long-running workload:
  `SimSnapshot.LogStaysBoundedUnderALongRunningWorkload` (`[kv-longrun]` line).
- [ ] Session dedupe survives snapshots: the KV campaign with snapshots on, and the
  sessionless-snapshot negative control.
- [ ] Debug and ASan green with zero warnings; clang-tidy, format, ruff, and pytest pass;
  final-commit CI green.

Every number comes from `results/m5/summary.json`.
