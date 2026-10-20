"""Measure simulator sensitivity, verify exact failing replay, and run a normal-node control."""

import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import re
import subprocess
import sys

from make_mutant import MUTATIONS
from verify_sim_replay import TRACE

INVARIANTS = (
    "two different leaders elected in the same term",
    "term regressed or durable vote changed within a term",
    "candidate crash loses announced term or self-vote",
    "Log Matching:",
    "State Machine Safety:",
    "Leader Completeness:",
    "committed entries lost",
    "committed entries reordered",
    "attempt to replace a committed log entry",
)
DETECTION = re.compile(r"^\[mutation-detected\] seed=(\d+) invariant=(.+)$", re.MULTILINE)


def failure(run: dict) -> tuple[int, str]:
    seeds = re.findall(r"\[sim-failure\] seed=(\d+) replay=", run["output"])
    traces = TRACE.findall(run["output"])
    if (
        run["exit"] != 1
        or len(seeds) != 1
        or len(traces) != 1
        or not traces[0]
        or not any(invariant in run["output"] for invariant in INVARIANTS)
        or "exceeded" in run["output"]
        or "did not converge" in run["output"]
        or "[  FAILED  ] ReplicationFuzz.CommittedHistorySurvivesSeededFaults" not in run["output"]
    ):
        raise ValueError("expected an invariant failure with one seed and replay trace")
    seed = int(seeds[0])
    if not 0 <= seed < 10000:
        raise ValueError("failure is outside the 10000-seed campaign")
    return seed, traces[0]


def verify(record: dict) -> dict:
    runs = record["runs"]
    if len(runs) not in (4, 5):
        raise ValueError("search, two replays, control, and optional survey required")
    signatures = [failure(run) for run in runs[:3]]
    if signatures[0] != signatures[1] or signatures[0] != signatures[2]:
        raise ValueError("failing traces do not replay exactly")
    seed, trace = signatures[0]
    control = runs[3]
    if (
        control["exit"] != 0
        or f"[replication] first_seed={seed} seeds=1 " not in control["output"]
        or "[  PASSED  ] 1 test." not in control["output"]
    ):
        raise ValueError("unmodified control must pass the same seed")
    report = {
        "seed": seed,
        "trace_sha256": hashlib.sha256(trace.encode()).hexdigest(),
        "failing_replays": 2,
        "control_passed": True,
    }
    if len(runs) == 5:
        survey = runs[4]
        match = re.search(
            r"\[mutation-survey\] first_seed=0 seeds=10000 detected=(\d+)", survey["output"]
        )
        detections = DETECTION.findall(survey["output"])
        if (
            survey["exit"] != 0
            or "[  PASSED  ] 1 test." not in survey["output"]
            or not match
            or int(match[1]) != len(detections)
            or not detections
            or len({item[0] for item in detections}) != len(detections)
        ):
            raise ValueError("survey must account for all 10000 seeds and every detection")
        for found_seed, invariant in detections:
            if not 0 <= int(found_seed) < 10000 or not any(
                invariant.startswith(prefix) for prefix in INVARIANTS
            ):
                raise ValueError("survey encountered an unexpected failure")
        if int(detections[0][0]) != seed:
            raise ValueError("survey and first-failure search disagree")
        report.update(survey_seeds=10000, detected_seeds=len(detections))
    return report


def read_evidence(text: str) -> dict:
    records = [json.loads(line) for line in text.splitlines() if line]
    if [record["mutation"] for record in records] != list(MUTATIONS):
        raise ValueError("each mutation must have exactly one ordered record")
    return {record["mutation"]: verify(record) for record in records}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--survey", action="store_true")
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    suffix = ".exe" if (args.build_dir / "quorumkv_tests.exe").exists() else ""
    watchdog = pathlib.Path(__file__).with_name("test_watchdog.py")

    def measure(mutation: str) -> dict:
        mutant = args.build_dir / f"quorumkv_mutant_{mutation}{suffix}"
        normal = args.build_dir / f"quorumkv_tests{suffix}"
        print(f"Measuring {mutation}", flush=True)

        def run(executable: pathlib.Path, *options: str) -> dict:
            command = [
                sys.executable,
                str(watchdog),
                str(executable.resolve(strict=True)),
                "--gtest_filter=ReplicationFuzz.*",
                *options,
            ]
            result = subprocess.run(
                command, capture_output=True, text=True, check=False, timeout=5460
            )
            return {
                "command": command,
                "exit": result.returncode,
                "output": result.stdout + result.stderr,
            }

        runs = [run(mutant, "--seed-count=10000")]
        record = {"mutation": mutation, "runs": runs}
        try:
            seed, _ = failure(runs[0])
            runs.extend(run(mutant, f"--seed={seed}") for _ in range(2))
            runs.append(run(normal, f"--seed={seed}"))
            if args.survey:
                runs.append(run(mutant, "--seed-count=10000", "--mutation-survey"))
            print(f"{mutation}: {json.dumps(verify(record))}", flush=True)
        except ValueError as error:
            record["verification_error"] = str(error)
        return record

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        with args.output.open("w", encoding="utf-8", newline="\n") as output:
            for record in pool.map(measure, MUTATIONS):
                output.write(json.dumps(record) + "\n")
                output.flush()
    read_evidence(args.output.read_text(encoding="utf-8"))
    print(f"PASS: every mutation detected and replayed; evidence: {args.output}", flush=True)


if __name__ == "__main__":
    main()
