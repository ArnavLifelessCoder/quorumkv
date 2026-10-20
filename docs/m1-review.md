# M1 review packet

Status: local acceptance criteria met; awaiting owner review and `approved`.
Work is on branch `m1`. Nothing has been pushed, and M2 has not started.

## What was built

- Portable C++20 CMake/Ninja project with debug, release, ASan, and Linux TSan presets.
  Project code uses warnings as errors. MSVC ASan removes incompatible runtime-check
  flags and disables incremental linking.
- Transport, Clock, Storage, and Random interfaces, plus a replaceable StateMachine
  interface. Durable storage ordering and snapshot/session responsibilities are
  documented; adapters and state-machine behavior remain later milestone work.
- Six peer message bodies: vote request/reply, append request/reply, and snapshot
  chunk request/reply. The envelope carries sender, recipient, term, and RPC ID.
- Versioned, length-prefixed, big-endian serialization with explicit tags, canonical
  boolean encoding, optional conflict terms, binary blobs, and bounded allocations.
- Boundary tests, truncated-frame tests, an independent reference encoder,
  deterministic round-trip fuzzing, malformed-input probes, seed replay, and
  command-line seed validation.
- Windows/Linux debug and ASan CI, an Ubuntu TSan job, and formatting/Python lint.
  All Python packages are installed into project-local virtual environments.

No RaftNode, simulator, real sockets, background threads, disk storage adapter,
KV implementation, client protocol, or benchmark is implemented in this milestone.
The protocol and interface details are in [protocol.md](protocol.md).

## Validation and full output

| Recorded run | GoogleTest tests | Seeds | Round trips | Corrupted/arbitrary probes | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| MSVC debug | 16 | 10,000 | 60,000 | 80,000 | Pass |
| MSVC AddressSanitizer | 16 | 10,000 | 60,000 | 80,000 | Pass |

Both runs cover seeds 0 through 9999. These are the same 10,000 unique seeds in
both configurations, not 20,000 distinct seeds. Each seed generates every peer
message type. Of the probes, 52,898 were structurally valid and re-encoded to the
same bytes, and 27,102 were rejected with CodecError in each configuration.
Both full-corpus fingerprints are `ddc03ddedd0aabdf`.

The replay script independently ran seed 42 twice. Both fingerprints were
`37c9f7593ffeef94`. An invalid negative CLI seed returned exit code 2.
These are codec fuzz results, not simulator safety checks. No simulator bug or
failing simulator seed is claimed.

Complete, unabridged test output and supporting evidence:

- [Debug test output](../results/m1/debug-tests.txt)
- [ASan test output](../results/m1/asan-tests.txt)
- [Replay output](../results/m1/replay.txt)
- [Formatting and Python lint output](../results/m1/lint.txt)
- [Machine-readable test summary](../results/m1/summary.json)
- [Debug configure output](../results/m1/debug-configure.txt),
  [build output](../results/m1/debug-build.txt), and
  [tool metadata](../results/m1/debug-tools.json)
- [ASan configure output](../results/m1/asan-configure.txt),
  [build output including flags](../results/m1/asan-build.txt), and
  [tool metadata](../results/m1/asan-tools.json)

The verified local tools were MSVC 19.44.35229, CMake 3.31.6-msvc6, Ninja 1.12.1,
and Python 3.13.15. The VS developer shell did not expose clang-cl, and no
clang-cl executable was found in the VS tool directories inspected. MSVC was
used for both required local builds. Formatting used clang-format 18.1.8 and
ruff 0.11.13 from `.venv`.

CI has not run because pushing requires approval. No Linux or CI-green result
is claimed. TSan is not a local Windows option; M1 has no threaded project code,
so a green TSan result is not an M1 acceptance gate. The Ubuntu job is configured
for the future threaded milestone gate.

## Acceptance checklist

- [x] CMake skeleton and required presets exist.
- [x] Windows/Linux CI is configured; Ubuntu TSan is included.
- [x] All four injected interfaces and the StateMachine interface exist.
- [x] Peer message types and versioned serialization exist.
- [x] Serialization round-trip fuzz tests pass under debug and ASan.
- [x] Failures include a seed replay command; deterministic replay is verified.
- [x] Formatting and Python lint pass; local builds emit no compiler warnings.
- [x] Spec files remain unchanged and ignored.
- [x] Local commits use the existing identity; no push has occurred.
- [ ] Owner approval received.

Simulator invariants, elections, Figure 8, crash persistence, sessions, ReadIndex,
snapshots, and threaded transport tests belong to later milestones.

## Files added or changed during M1

Build and repository setup:
`.gitignore`, `.gitattributes`, `.clang-format`, `.clang-tidy`, `CMakeLists.txt`,
`CMakePresets.json`, `pyproject.toml`, `.github/workflows/ci.yml`.

Public contracts and implementation:
`include/quorumkv/types.hpp`, `include/quorumkv/interfaces.hpp`,
`include/quorumkv/codec.hpp`, `src/rpc/codec.cpp`.

Tests:
`tests/toolchain_test.cpp`, `tests/interfaces_test.cpp`, `tests/codec_test.cpp`,
`tests/codec_fuzz_test.cpp`, `tests/reference_codec.hpp`, `tests/seed_config.hpp`,
`tests/main.cpp`.

Tooling and documentation:
`scripts/check_format.py`, `scripts/test.ps1`, `scripts/verify_replay.py`,
`README.md`, `docs/protocol.md`, `docs/m1-review.md`.

Recorded evidence:
`results/m1/debug-configure.txt`, `results/m1/debug-build.txt`,
`results/m1/debug-tests.txt`, `results/m1/debug-tools.json`,
`results/m1/asan-configure.txt`, `results/m1/asan-build.txt`,
`results/m1/asan-tests.txt`, `results/m1/asan-tools.json`,
`results/m1/lint.txt`, `results/m1/replay.txt`, `results/m1/summary.json`.

The spec, build trees, downloaded GoogleTest sources, tool caches, and `.venv`
are excluded from Git.
