# M3 implementation and review gates

M2 is approved at `21e5949`; work for M3 stays on `m3` until approval.

Implementation order:

1. Atomic storage batches and validated contiguous log suffix operations.
   Before-sync failure must discard the complete pending log/hard-state batch;
   after-sync failure must retain the complete durable batch.
2. Persistent leader no-op, AppendEntries consistency and truncation, conflict
   term/index hints, per-peer replication progress, and current-term-only
   majority commit advancement. Expose ordered committed entries to the driver.
3. Simulator proposals, crash/restart log images, application histories, and
   historical State Machine Safety, Log Matching, Leader Completeness, and
   committed-prefix checks after every event.
4. Directed Figure 8 tests, duplicate/reordered response tests, seeded
   replication and recovery campaigns, and exact failing-seed replay.
5. Filesystem storage behind portable Storage and platform file sync/rename
   adapters, with crash-safe publication and corruption validation.
6. Test-only mutation libraries; full-campaign per-mutation detection counts;
   candidate crash at the RequestVote send boundary; per-test CTest deadlines
   and external watchdog diagnostics for hangs after a passing test.
7. Clean-source debug/ASan/tidy capture, results-only commit, and all CI jobs
   green. M4 waits for owner approval.

RaftNode continues to depend only on Transport, Clock, Storage, and Random.
The simulator drives application of committed entries to an ordered test model;
the KV command/session implementation remains M4 behind StateMachine.

The directed Figure 8 schedule follows the
[extended Raft paper](https://raft.github.io/raft.pdf), section 5.4.2.
It must distinguish replication of an older-term entry on a majority from
commitment after a current-term entry reaches a majority.
