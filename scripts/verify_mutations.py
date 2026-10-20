"""Require real election-safety failures and identical replay from both test-only mutants."""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess

from verify_sim_replay import TRACE, signature

MUTATIONS = ("count_stale_votes", "reply_before_persist")
INVARIANT = "two different leaders elected in the same term"


def failure_signature(code: int, text: str) -> tuple[int, str]:
    seeds = re.findall(r"\[sim-failure\] seed=(\d+) replay=", text)
    traces = TRACE.findall(text)
    if (
        code != 1
        or len(seeds) != 1
        or len(traces) != 1
        or not traces[0]
        or INVARIANT not in text
        or "leader election exceeded" in text
        or "[  FAILED  ] ElectionFuzz.SafetyAndRecoveryAcrossSeededFaultSchedules" not in text
    ):
        raise ValueError("mutation must fail the historical leader invariant with a replay trace")
    return int(seeds[0]), traces[0]


def verify_runs(runs: list[dict]) -> dict:
    if len(runs) != 4:
        raise ValueError(
            "expected a search, two failing replays, and a passing normal-node control"
        )
    failures = [failure_signature(run["exit"], run["output"]) for run in runs[:3]]
    if any(failure != failures[0] for failure in failures):
        raise ValueError("mutation search and replay traces differ")
    seed, trace = failures[0]
    if not 0 <= seed < 10000:
        raise ValueError("failing seed is outside the mutation search range")
    control = runs[3]
    if control["exit"] != 0 or "[  PASSED  ] 1 test." not in control["output"]:
        raise ValueError("unmodified node must pass the failing seed")
    _, summary = signature(control["output"])
    if not summary.startswith(f"[sim] first_seed={seed} seeds=1 "):
        raise ValueError("control did not replay the mutant's failing seed")
    return {
        "seed": seed,
        "searched_seeds": seed + 1,
        "failing_replays": 2,
        "invariant": INVARIANT,
        "trace_sha256": hashlib.sha256(trace.encode()).hexdigest(),
        "control_passed": True,
    }


def read_evidence(text: str) -> dict:
    # JSON records preserve complete stdout/stderr and exit codes, including failed runs.
    records = [json.loads(line) for line in text.splitlines() if line]
    if [record["mutation"] for record in records] != list(MUTATIONS):
        raise ValueError("expected both mutation records exactly once")
    return {record["mutation"]: verify_runs(record["runs"]) for record in records}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    suffix = ".exe" if (args.build_dir / "quorumkv_tests.exe").exists() else ""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as log:
        for mutation in MUTATIONS:
            mutant = args.build_dir / f"quorumkv_mutant_{mutation}{suffix}"
            normal = args.build_dir / f"quorumkv_tests{suffix}"

            def run(executable: pathlib.Path, *options: str) -> dict:
                command = [
                    str(executable.resolve(strict=True)),
                    "--gtest_filter=ElectionFuzz.*",
                    *options,
                ]
                result = subprocess.run(
                    command, capture_output=True, text=True, check=False, timeout=1800
                )
                return {
                    "command": command,
                    "exit": result.returncode,
                    "output": result.stdout + result.stderr,
                }

            runs = [run(mutant, "--seed-count=10000")]
            # Keep unsuccessful sensitivity checks too; never replace an unexpected pass.
            try:
                seed, _ = failure_signature(runs[0]["exit"], runs[0]["output"])
                runs.extend(run(mutant, f"--seed={seed}") for _ in range(2))
                runs.append(run(normal, f"--seed={seed}"))
                report = verify_runs(runs)
            finally:
                log.write(json.dumps({"mutation": mutation, "runs": runs}) + "\n")
                log.flush()
            print(f"{mutation}: {json.dumps(report)}", flush=True)
    print(f"PASS: both mutations fail safety and replay identically. Full output: {args.output}")


if __name__ == "__main__":
    main()
