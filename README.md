# quorumkv

A C++20 Raft replicated key-value store under construction. The core will run behind
injected transport, clock, storage, and random interfaces so the same logic can be
tested with deterministic failures and used with a TCP transport. The initial state
machine will use `std::map` behind a replaceable interface.

M1 is in progress. Only the build scaffold is present; no Raft algorithm or server
is implemented, and no performance results have been collected.

## Build and test

Use CMake 3.20 or newer, Ninja, and Python 3.11 or newer. Windows requires Visual
Studio 2022 C++ Build Tools with the Windows SDK and C++ AddressSanitizer component.
Run the following from an x64 developer shell with `cl` on PATH. Linux requires
GCC 12 or newer, or Clang 15 or newer.

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
The Linux-only `tsan` preset is reserved for concurrency validation. There is no
threaded project code yet, so a TSan CI job will be added with the threaded milestone.
The `release` preset is reserved for benchmarks.

Formatting and Python lint:

```text
python -m pip install clang-format==18.1.8 ruff==0.11.13
python scripts/check_format.py
ruff check .
ruff format --check .
```

## Bugs found by the simulator

The simulator has not been implemented. No simulator bug discoveries or seed
results are claimed.
