# M7 review packet

Status: complete. The checker is on branch `m6`, where the M6 acceptance campaign uses it.

## Built

`python/linearizability/checker.py`, described in [the checker design](linearizability.md):

- Wing-Gong / Lowe search with memoization over (linearized set, register state).
- P-compositional: each key is checked separately against a register model of put, append,
  cas and get.
- `info` writes may take effect at any point after their invocation, or never. `fail`
  operations and `info` reads are dropped.
- On failure, the history is shrunk by a binary search for the earliest failing prefix, then
  delta-debugging chunk removal, then single-operation removal. Writes whose values remaining
  observations show are kept. The report names the operation that cannot be placed and every
  register state a real-time order allows at that point.

## Acceptance

| Gate | Evidence |
| --- | --- |
| Known-linearizable hand-crafted histories pass | `tests/python/test_linearizability.py` |
| Known-non-linearizable hand-crafted histories fail | Same file: stale read, duplicated append, CAS that lies, value read before its write |
| Agrees with an independent reference | 3,000 random small histories compared against a brute-force checker over every real-time order |
| A deliberately broken store mode is flagged | Recorded `--unsafe-reads` history fixture, plus all 3 campaign runs with `--unsafe-reads` flagged (`results/m6/recheck.json`) |
| Minimal counterexample | Unsafe-run counterexamples have 3, 3 and 4 operations |
| Checks a five-minute run quickly | At most 0.75 s per campaign history |

A checker that never fails would fail the unsafe-reads gate; that gate is part of the
campaign script's exit status.
