"""Re-run the checker over every recorded M6 campaign history and record the verdicts.

    python scripts/recheck_m6.py --campaign build/m6-campaign

Each history must match the SHA-256 recorded when the campaign ran, so the verdicts in
results/m6/recheck.json describe exactly those runs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from linearizability.checker import check_history  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", type=pathlib.Path, required=True)
    parser.add_argument("--results", type=pathlib.Path, default=ROOT / "results" / "m6")
    args = parser.parse_args()
    campaign = json.loads((args.results / "campaign.json").read_text())
    verdicts = []
    for unsafe, runs in ((False, campaign["runs"]), (True, campaign["unsafe_runs"])):
        for run in runs:
            name = f"{'unsafe' if unsafe else 'seed'}-{run['seed']}"
            data = (args.campaign / name / "history.jsonl").read_bytes()
            if hashlib.sha256(data).hexdigest() != run["history_sha256"]:
                raise ValueError(f"{name}: history does not match the recorded hash")
            history = [json.loads(line) for line in data.decode().splitlines() if line.strip()]
            started = time.monotonic()
            verdict = check_history(history)
            entry = {
                "run": name,
                "unsafe_reads": unsafe,
                "operations": len(history),
                "linearizable": verdict.ok,
                "check_seconds": round(time.monotonic() - started, 2),
            }
            if not verdict.ok:
                entry["counterexample"] = verdict.explanation
                entry["counterexample_operations"] = (
                    sum(line.startswith("  client") for line in verdict.explanation.splitlines())
                    - 1
                )
            verdicts.append(entry)
            print(json.dumps({k: entry[k] for k in ("run", "linearizable", "operations")}))
    report = {
        "producer": "scripts/recheck_m6.py",
        "runs": verdicts,
        "all_safe_linearizable": all(v["linearizable"] for v in verdicts if not v["unsafe_reads"]),
        "all_unsafe_flagged": all(not v["linearizable"] for v in verdicts if v["unsafe_reads"]),
    }
    (args.results / "recheck.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("all_safe_linearizable", "all_unsafe_flagged")}))


if __name__ == "__main__":
    main()
