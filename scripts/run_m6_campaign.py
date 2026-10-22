"""Run the M6/M7 acceptance campaign against real server processes.

    python scripts/run_m6_campaign.py --bin build/release --out build/m6-campaign

Twenty five-minute runs with different seeds (odd seeds use 3 nodes, even seeds 5) must all
complete and pass the linearizability checker. Three runs with --unsafe-reads must all be
flagged. results/m6/ receives each run's summary and nemesis log plus the SHA-256 of its full
history, which stays in the campaign directory because of its size.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))


def machine() -> dict:
    info = {
        "platform": platform.platform(),
        "processor": platform.processor(),
        "python": platform.python_version(),
    }
    try:
        import psutil

        info["logical_cpus"] = psutil.cpu_count(logical=True)
        info["physical_cpus"] = psutil.cpu_count(logical=False)
        info["memory_gib"] = round(psutil.virtual_memory().total / 2**30, 1)
    except ImportError:
        pass
    return info


def one_run(binaries: pathlib.Path, out: pathlib.Path, seed: int, duration: float, unsafe: bool):
    suffix = ".exe" if sys.platform == "win32" else ""
    command = [
        sys.executable,
        "-m",
        "harness.run",
        "--server", str(binaries / f"quorumkv_server{suffix}"),
        "--client", str(binaries / f"quorumkv_client{suffix}"),
        "--seed", str(seed),
        "--duration", str(duration),
        "--nodes", "3" if seed % 2 else "5",
        "--out", str(out),
    ]  # fmt: skip
    if unsafe:
        command.append("--unsafe-reads")
    started = time.monotonic()
    result = subprocess.run(command, cwd=ROOT / "python", capture_output=True, text=True)
    summary = json.loads((out / "run.json").read_text())
    summary["wall_seconds"] = round(time.monotonic() - started, 1)
    summary["exit_code"] = result.returncode
    summary["history_sha256"] = hashlib.sha256((out / "history.jsonl").read_bytes()).hexdigest()
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument("--duration", type=float, default=300)
    parser.add_argument("--unsafe-runs", type=int, default=3)
    parser.add_argument("--unsafe-duration", type=float, default=60)
    parser.add_argument("--results", type=pathlib.Path, default=ROOT / "results" / "m6")
    args = parser.parse_args()
    args.results.mkdir(parents=True, exist_ok=True)
    report = {"producer": "scripts/run_m6_campaign.py", "machine": machine(), "runs": []}
    report["unsafe_runs"] = []
    plan = [(seed, False) for seed in range(1, args.runs + 1)]
    plan += [(100 + seed, True) for seed in range(1, args.unsafe_runs + 1)]
    for seed, unsafe in plan:
        name = f"{'unsafe' if unsafe else 'seed'}-{seed}"
        out = args.out / name
        summary = one_run(
            args.bin, out, seed, args.unsafe_duration if unsafe else args.duration, unsafe
        )
        (report["unsafe_runs"] if unsafe else report["runs"]).append(summary)
        for file in ("run.json", "nemesis.jsonl"):
            shutil.copyfile(out / file, args.results / f"{name}-{file}")
        print(json.dumps({k: summary.get(k) for k in ("seed", "linearizable", "operations")}))
        (args.results / "campaign.json").write_text(json.dumps(report, indent=2) + "\n")
    safe = report["runs"]
    report["verdict"] = {
        "runs": len(safe),
        "all_linearizable": all(run["linearizable"] is True for run in safe),
        "operations": sum(run["operations"] for run in safe),
        "kills": sum(run["nemesis"].get("kill", 0) for run in safe),
        "pauses": sum(run["nemesis"].get("pause", 0) for run in safe),
        "partitions": sum(run["nemesis"].get("partition", 0) for run in safe),
        "unsafe_runs": len(report["unsafe_runs"]),
        "unsafe_all_flagged": all(run["linearizable"] is False for run in report["unsafe_runs"]),
    }
    (args.results / "campaign.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["verdict"]))
    ok = report["verdict"]["all_linearizable"] and report["verdict"]["unsafe_all_flagged"]
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
