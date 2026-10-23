# Benchmarks (M8)

All benchmarks run on one machine against localhost clusters of real `quorumkv_server`
processes (release build). The machine, compiler, and flags are recorded in
`results/m8/benchmarks.json` with every result. There is no second machine, so every number
includes loopback networking and all nodes sharing one CPU and one disk.

## How they run

`bench/run_benchmarks.py` starts a fresh cluster for every configuration. Peers connect
directly to each other, without the fault proxies. The load comes from `quorumkv_load`:
32 client threads, each with its own `Client` and session, run closed-loop for 10 seconds
after a 2-second warm-up. Each thread waits for its operation to finish before starting the
next. Only operations that start and finish inside the measured window count. Keys are drawn
uniformly from 1,000.

Each throughput configuration runs at least five times. The report gives the median
throughput with min and max, and the median of each run's p50 and p99 latency. Threads are
not pinned to cores, and turbo boost is not disabled; both are recorded with the results.

| Benchmark | What changes |
| --- | --- |
| Write vs cluster size | 3 and 5 nodes |
| Batching and pipelining | `--no-batching` proposes one command per durable write, one entry per AppendEntries; `--no-pipeline` keeps one AppendEntries in flight per follower |
| ReadIndex vs log reads | `Client::Read` vs `Get` proposed through the log |
| Value size | 100 B, 1 KB, 10 KB |
| Failover | SIGKILL the leader while one client writes back to back; time from the kill to the completion of the first write invoked after the process has exited. 50 trials, with a fresh cluster every 10. |
| Snapshot catch-up | Kill a follower, write at least 1,000,000 entries with 64 client threads, restart it, and time until its applied index reaches the leader's commit index. The follower is far behind the leader's snapshot, so it catches up through InstallSnapshot. |

Python's `time.monotonic_ns` and the C++ `steady_clock` share one time base on the test
machine. This was checked by bracketing a `quorumkv_load --mode clock` call between two
Python readings. That shared base lets the failover benchmark compare the kill time with the
probe's completion timestamps directly.

## Storage and batching

Before M8, every durable write rewrote the whole retained log image. The storage now appends
one checksummed journal record per batch and compacts it into the image occasionally (see
`include/quorumkv/file_storage.hpp`). With batching on, all commands that arrive in one
server event-loop pass share one journal record and one fsync, and one AppendEntries carries
up to 256 entries.

`bench/report.py` renders the README results table and the PNG plots from
`results/m8/benchmarks.json`.
