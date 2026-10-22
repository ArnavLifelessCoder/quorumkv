# M6 review packet

Status: implementation and the acceptance campaign are complete on branch `m6`.

## Built

- `quorumkv_server`: one Raft node per process on a single event-loop thread, with
  FileStorage, the KV store, and real clock and random adapters. Peer and client listeners
  are separate, and length-prefixed frames carry tags 1 to 6 (peers) and 7 to 8 (clients).
  See [the networking design](networking.md).
- `quorumkv::Client` and `quorumkv_client`: leader discovery through `NotLeader` hints,
  retries that keep one session sequence, and `ok`, `fail` and `unknown` outcomes. The
  `workload` mode writes JSONL histories on the system-wide monotonic clock.
- `python/harness/`:
  - `proxy.py`: one TCP proxy per directed peer link;
  - `cluster.py`: starts 3 or 5 server processes;
  - `run.py`: the nemesis, which partitions and cuts links, adds link delays, SIGKILLs the
    leader and restarts it, and pauses processes. See [the harness](linearizability.md).
- `tests/server_test.cpp`: three servers on separate threads over real TCP, covering failover
  and restart from disk. It runs under TSan in Linux CI.
- CI runs a short real-cluster nemesis run in the debug jobs on Windows and Linux.

## Acceptance

From `results/m6/campaign.json` (`scripts/run_m6_campaign.py`), re-checked with the final
checker in `results/m6/recheck.json` (`scripts/recheck_m6.py`, which verifies each history's
SHA-256 first). Provenance is in `results/m6/provenance.json`.

| Gate | Result |
| --- | --- |
| 20 five-minute runs with different seeds complete | 20 of 20 (seeds 1 to 20; odd seeds 3 nodes, even seeds 5) |
| All histories pass the M7 checker | 20 of 20 linearizable |
| Operations checked | 703,109 (10,692 to 44,771 per run) |
| Nemesis events across the 20 runs | 389 leader kills with restart, 180 pauses, 414 partitions, plus one-way cuts and delays |
| Outcomes recorded as `info` / `fail` | 801 / 801 (counts differ per run; the totals coincide) |

Each run lasted 300 seconds of nemesis followed by 10 seconds of healed recovery. The
machine is recorded in `campaign.json`: a 16-core, 32-thread AMD processor with 15.1 GiB
of memory, running Windows 11.

## Notes

- On Windows, SIGKILL is `TerminateProcess`, and SIGSTOP/SIGCONT are psutil thread
  suspend and resume. The Linux CI smoke run uses the real signals.
- Peer messages queued while a link is down are dropped, as network loss would drop them.
- Full histories (several MB each) are not committed; their hashes are.
