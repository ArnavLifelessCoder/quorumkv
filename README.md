# quorumkv

A C++20 Raft replicated key-value store under construction. The core will run behind
injected transport, clock, storage, and random interfaces so the same logic can be
tested with deterministic failures and used with a TCP transport. The initial state
machine will use `std::map` behind a replaceable interface.

M1 provides injectable interfaces, peer message types, and a bounded binary codec
with deterministic fuzz tests. No Raft algorithm or server is implemented, and no
performance results have been collected.

## Build and test

Use CMake 3.20 or newer, Ninja, and Python 3.11 or newer. Windows requires Visual
Studio 2022 C++ Build Tools with the Windows SDK and C++ AddressSanitizer component.
The Windows helper enters the x64 developer shell and saves complete configure,
build, and test output under `results/m1/`:

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
python -m pip install --no-cache-dir clang-format==18.1.8 ruff==0.11.13
python scripts/check_format.py
ruff check .
ruff format --check .
```

On Linux, activate with `source .venv/bin/activate` before the same pip and lint
commands. Python packages belong in `.venv/`.

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
The `StateMachine` interface is ready for an in-memory `std::map` implementation
in M4. The LSM engine is not required to build this project.

## Bugs found by the simulator

The simulator has not been implemented. No simulator bug discoveries or seed
results are claimed.
