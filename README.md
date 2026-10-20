# quorumkv

A C++20 Raft replicated key-value store under construction. The core will run behind
injected transport, clock, storage, and random interfaces so the same logic can be
tested with deterministic failures and used with a TCP transport. The initial state
machine will use `std::map` behind a replaceable interface.

M2 adds leader election and a deterministic fault simulator to the injectable
interfaces and bounded peer-message codec. The node handles durable votes, terms,
timeouts, and heartbeats. Log replication, the KV implementation, and real
networking are later milestones. No performance results have been collected.

## Build and test

Use CMake 3.20 or newer, Ninja, and Python 3.11 or newer. Windows requires Visual
Studio 2022 C++ Build Tools with the Windows SDK and C++ AddressSanitizer component.
The Windows helper enters the x64 developer shell and saves complete configure,
build, and test output under `build/test-logs/`:

```powershell
.\scripts\test.ps1 -Preset debug
.\scripts\test.ps1 -Preset asan
```

Use `-VsRoot` if Build Tools is installed elsewhere. The ASan runtime DLL must
remain on the developer shell's PATH when running a test binary directly.

On Linux, use GCC 12 or newer or Clang 15 or newer. The following also works in a
Windows x64 developer shell:

```text
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --verbose
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --verbose
```

GoogleTest is fetched at configure time. Internet access is required for the first
configuration. Build products and downloaded dependencies stay under `build/`.
Windows uses AddressSanitizer; Linux uses AddressSanitizer and UndefinedBehaviorSanitizer.
The Linux-only `tsan` preset also runs in CI. There is no threaded project code yet;
threaded milestones will require its CI result to be green before completion.
The `release` preset is reserved for benchmarks.

Formatting and Python lint use a project-local virtual environment. On Windows:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --no-cache-dir clang-format==18.1.8 ruff==0.11.13 pytest==8.4.2
python scripts/check_format.py
ruff check .
ruff format --check .
python -m pytest tests/python -q --basetemp=build/pytest
```

On Linux, activate with `source .venv/bin/activate` before the same pip and lint
commands. Python packages belong in `.venv/`.

Run `python scripts/check_tidy.py` from the VS developer shell after configuring
debug. It checks every tracked C++ translation unit with `.clang-tidy`, including
project headers, and saves complete output to `build/review/clang-tidy.txt`.
Use `--clang-tidy` to specify an executable when it is not on PATH or available
through the VS developer environment. CI also runs clang-tidy and retains its log.

## Reproducible milestone evidence

Commit all source, configuration, and documentation changes first. With a clean
tree and the project-local Python tools installed, run:

```powershell
.\scripts\capture_m1.ps1
```

Use `scripts/capture_m2.ps1` for the current milestone; M1 evidence is retained
unchanged. The capture runs debug and ASan with individually discovered GoogleTest cases, clang-tidy,
replay verification, Python regression tests, formatting, and lint. All intermediate
output goes into an ignored `build/m1-capture-*` directory. The script verifies
that HEAD and the clean tree have not changed before publishing `results/m1/`.

`scripts/summarize_m1.py` derives `summary.json` from the captured debug, ASan, and
replay logs, validating that both builds identify the same clean source commit.
It rejects missing/failed tests, mismatched seed corpora, and invalid provenance.
To regenerate only that JSON from existing valid logs:

```powershell
.\.venv\Scripts\python.exe scripts/summarize_m1.py
```

Commit the published evidence in a separate results-only commit. Its
`source_revision` identifies the preceding tested source HEAD; the evidence commit
must change no files outside `results/m1/`. This avoids a self-referential commit
hash. M1 was approved after all jobs passed in
[CI run 39425744602](https://github.com/ArnavLifelessCoder/quorumkv/actions/runs/39425744602)
on `5cbbb40`. M2 is authorized.

## Tests and replay

The suite checks every message kind, exact wire bytes, all truncated prefixes,
noncanonical booleans, unknown tags, oversized lengths, and exact allocation
boundaries. Seeded tests compare the codec against an independent, simple encoder,
round-trip every message kind, and probe corrupted and arbitrary frames. The
generator uses fixed integer arithmetic so a seed produces the same corpus across
toolchains. Complete logs are in `results/m1/`; the review packet records actual
seed counts and results.

To replay one seed from a developer shell:

```powershell
.\build\debug\quorumkv_tests.exe --seed=42 --gtest_filter=CodecFuzz.*
```

Add `--seed-count=N` to run a consecutive range starting at the specified seed.
Failures print the exact replay command. The test emits a corpus fingerprint to
check reproducibility:

```powershell
.\.venv\Scripts\python.exe scripts/verify_replay.py build/debug/quorumkv_tests.exe
```

See [the protocol and interface contracts](docs/protocol.md) for field layout,
limits, persistence ordering, and the boundary between the core and adapters.
The [client codec plan](docs/client-protocol-plan.md) specifies the planned
ClientRequest, ClientResponse, and NotLeader leader-hint extension before M4.
The `StateMachine` interface is ready for an in-memory `std::map` implementation
in M4. The LSM engine is not required to build this project.

## Election simulation

`RaftNode` receives only Transport, Clock, Storage, and Random dependencies.
`Step(Message)` processes one message and `Tick()` observes the injected logical
clock. Its durable term and vote are saved before sending election messages.
The simulator supplies a virtual clock, independent seeded random streams,
queued delivery, packet faults, partitions, and crash/restart storage.

The campaign checks election safety after each simulated event and retains
leader history across step-downs and restarts. It also checks that terms never
decrease and a durable vote never changes within a term. Liveness is tested
with a connected majority; arbitrary partitions can prevent elections.
The [M2 review packet](docs/m2-review.md) links complete logs, measured election
bounds, fault counts, and clean-source evidence.

Replay a simulator seed from a developer shell:

```powershell
.\build\debug\quorumkv_tests.exe --gtest_filter=ElectionFuzz.* --seed=42
.\.venv\Scripts\python.exe scripts/verify_sim_replay.py build/debug/quorumkv_tests.exe --output build/review/sim-replay.txt
```

A single seed emits a canonical event trace. On failure, the campaign prints
the seed, replay command, and trace. The replay script compares the complete
trace and outcome; `--compare-executable` also checks another build. Capture
compares debug and ASan and generates `results/m2/summary.json` through
`scripts/summarize_m2.py`. See [the simulator design](docs/election-simulator.md)
for event semantics and current limitations.

## Bugs found by the simulator

No implementation bug has been found by the election seed campaign so far.
The safety checker is tested against deliberately conflicting leader histories
and lost votes; those negative tests are not reported as discovered bugs.
