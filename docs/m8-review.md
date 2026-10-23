# M8 review packet

Status: complete on branch `m8`. Benchmark results are in `results/m8/benchmarks.json`
(`bench/run_benchmarks.py`), rendered with `bench/report.py`. Methodology is in
[the benchmark notes](benchmarks.md). The code's CI evidence is the exact-commit run for
`07350df` ([run 39947819043](https://github.com/ArnavLifelessCoder/quorumkv/actions/runs/39947819043)),
recorded in `results/m8/ci.json`: all seven jobs passed, each test job ran 117 tests, and the
campaign summaries are identical across all five test jobs and equal to M5's.

## Built

- `bench/load.cpp` (`quorumkv_load`): closed-loop multi-threaded load generator, failover
  probe, and clock alignment check.
- `bench/run_benchmarks.py`: runs every configuration on fresh localhost clusters, at least
  five times each, and records machine information with the results.
- `bench/report.py`: renders the README table and `results/m8/*.png` from the JSON.
- Group commit (`RaftNode::ProposeBatch`, server-side batching per event-loop pass) and a
  pipelining switch (`--no-pipeline`), with `--no-batching` to measure their effect.
- `FileStorage` now appends checksummed journal records instead of rewriting the whole log
  image on every write. The old image path remains for compaction and snapshots.
  `tests/file_storage_test.cpp` covers crash points, torn tails, corruption, and a
  differential test against the in-memory store at three journal limits.

## Benchmarks required by the spec

| Benchmark | Where |
| --- | --- |
| Write throughput and p50/p99 latency, 3 vs 5 nodes | `write_3_nodes`, `write_5_nodes` |
| Batching and pipelining on vs off | `batching_on_pipelining_on`, `batching_off`, `pipelining_off`, `batching_and_pipelining_off` |
| ReadIndex vs reads through the log | `read_index`, `read_through_log` |
| Failover time, median and p99 over 50 trials | `failover` |
| Snapshot catch-up for a follower 1M entries behind | `snapshot_catch_up` |
| Value sizes 100 B, 1 KB, 10 KB | `value_100_b`, `value_1_kb`, `value_10_kb` |

The numbers themselves are in the README results table, generated from the JSON.

## Notes

- The benchmark driver's later commits change only `bench/` and docs, not the server.
- The first failover runs included three samples of about 3 ms: writes the old leader
  acknowledged between the timestamp and the kill taking effect. The driver now counts only
  writes invoked after the process has exited, and also records when a new leader appears.
- Catch-up runs once crashed on a status request that timed out after 30 s, probably while the
  restarted follower was installing its snapshot. A timed-out request now counts as no answer.

- All nodes share one machine, one CPU and one disk; see Limitations in the README.
- Threads are not pinned and turbo boost is not disabled. Both facts are recorded in the
  results.
