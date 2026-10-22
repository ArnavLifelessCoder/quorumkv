# Fault harness and linearizability checker (M6, M7)

## Harness

`python/harness/` starts real `quorumkv_server` processes on localhost:

- `proxy.py` gives every directed peer link i -> j its own TCP proxy. Node i dials the proxy,
  and replies from j travel over j's own link j -> i. Blocking one proxy cuts one direction;
  blocking both partitions a pair. No root and no firewall rules are involved.
- `cluster.py` allocates ports, starts and restarts nodes, kills them, pauses and resumes them,
  and partitions, delays or cuts links.
- `run.py` starts client processes (`quorumkv_client workload`), then runs the nemesis every
  2 to 5 seconds for the run's duration. The nemesis chooses between random two-sided
  partitions, one-way cuts, link delays of 50 to 400 ms, SIGKILL of the current leader followed
  by a restart 1 to 3 seconds later, and pauses of 1 to 3 seconds. At the end it heals
  everything, resumes and restarts nodes, lets clients finish, merges the client histories by
  invocation time, and runs the checker.

Each client logs one JSON line per operation, with timestamps from the system-wide monotonic
clock, so histories from separate processes share one time line:

```json
{"client": 3, "op": "cas", "key": "k1", "value": "c3-41", "expected": "c2-17",
 "invoke_ts": 75504459607600, "complete_ts": 75504495167700, "result": "ok",
 "output": {"exchanged": true, "found": true, "previous": "c2-17"}}
```

`result` is `ok`, `fail` (definitely had no effect), or `info` (may or may not have had one).

## Checker

`python/linearizability/checker.py` checks each key separately, which is valid because a map of
registers is P-compositional. Each key is checked against a register model:

| Operation | Model |
| --- | --- |
| `put(v)` | state becomes `v` |
| `append(s)` | state becomes `(state or "") + s` |
| `cas(e, n)` | if the key exists and equals `e`: state becomes `n`, exchanged; otherwise unchanged. `found` and `previous` must match. |
| `get` | `found` and `value` must match the state |

`fail` operations are dropped. `info` writes have no return event: they may be linearized at
any point after their invocation, or not at all. `info` reads are dropped.

The search is Lowe's iterative Wing-Gong algorithm. It walks the call/return event list and
linearizes the first call whose model step reproduces its recorded output. Pairs of
(linearized set, state) that already failed are memoized, and the search backtracks when it
reaches the return of an operation it has not linearized.

### Counterexamples

On failure the checker shrinks the history before reporting it:

1. Linearizability is prefix-closed, so a binary search finds the earliest failing cut.
   Operations still running at the cut become `info`.
2. Operations are then removed greedily until nothing more can go. Writes whose values a
   remaining read or CAS observed are kept, so every observation stays explained.

The report lists the remaining operations and the operation that cannot be placed. It also
gives every register state that a real-time-respecting order allows at that point. From a
real `--unsafe-reads` run:

```text
key 'k0': no order consistent with real time explains these operations:
  client 3 put(k0, 'c3-133') [75508249105200, 75508322742200]
  client 3 append(k0, 'c3-288;') [75518764335500, 75518822380400]
  client 4 get(k0) -> 'c3-133' [75531915684700, 75531915906500]
  client 4 get(k0) -> 'c3-133' [...] cannot be placed: 2 operation(s) finished before it
  started, and every valid order leaves the register in one of 'c3-133c3-288;' at that
  point, none of which matches its result.
```

### How the checker is tested

- Hand-written linearizable and non-linearizable histories, including `info` semantics, CAS
  outputs, independent keys, and a duplicated append.
- A differential test against a brute-force checker that tries every real-time-respecting
  order, on 3,000 random small histories. Both verdicts occur, and many histories include
  `info` operations.
- Recorded histories from the real cluster: one with ReadIndex reads, which must pass, and one
  with `--unsafe-reads`, which must be flagged with a counterexample of at most six operations.
- The M6 campaign runs extra `--unsafe-reads` runs and requires every one to be flagged. A
  checker that never fails would fail this gate.
