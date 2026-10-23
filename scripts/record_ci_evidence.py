"""Record a milestone's evidence from its exact-commit CI run.

    python scripts/record_ci_evidence.py --branch m5 --revision <sha> --out results/m5

The run must exist for that branch and commit, and all seven jobs must have succeeded. For
every test job (Windows and Linux debug and ASan, Linux TSan) this saves the CTest totals and
every campaign summary line the suite prints, so each number in results/ traces to one CI
log. Full test-job logs are saved next to the summary.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import time

from fetch_ci import credential, request

SUMMARY = re.compile(
    r"\[(fuzz|sim|replication|kv|kv-snapshot|kv-longrun|kv-negative|kv-snapshot-negative)\] "
    r"([^\r\n]+)"
)
# CTest omits the failure count when nothing failed.
TOTALS = re.compile(r"(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)")
STAMP = re.compile(r"^\d{4}-\d\d-\d\dT[\d:.]+Z ?")


def parse_job(log: str) -> dict:
    lines = [STAMP.sub("", line) for line in log.splitlines()]
    totals = [TOTALS.search(line) for line in lines]
    totals = [match.groups() for match in totals if match]
    if len(totals) != 1:
        raise ValueError("expected exactly one CTest summary in the job log")
    percent, failed, total = (int(value or 0) for value in totals[0])
    if percent != 100 or failed != 0:
        raise ValueError("a CTest case failed")
    summaries: dict[str, list[str]] = {}
    for line in lines:
        for kind, fields in SUMMARY.findall(line):
            summaries.setdefault(kind, []).append(fields.strip())
    return {"tests": total, "summaries": {kind: sorted(set(v)) for kind, v in summaries.items()}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--wait-seconds", type=int, default=0)
    args = parser.parse_args()
    # The API filters on full commit hashes only.
    revision = subprocess.run(
        ["git", "rev-parse", args.revision], capture_output=True, text=True, check=True
    ).stdout.strip()
    token = credential()
    deadline = time.monotonic() + args.wait_seconds
    while True:
        runs = request(f"actions/runs?branch={args.branch}&head_sha={revision}", token)
        matches = [
            run
            for run in runs["workflow_runs"]
            if run["head_sha"] == revision and run["name"] == "CI"
        ]
        if matches and matches[0]["status"] == "completed":
            run = matches[0]
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("no completed CI run for that branch and revision")
        time.sleep(60)
    jobs = request(f"actions/runs/{run['id']}/jobs?per_page=100", token)["jobs"]
    if run["conclusion"] != "success" or len(jobs) != 7:
        raise RuntimeError(f"CI gate failed: {run['html_url']}")
    if any(job["conclusion"] != "success" for job in jobs):
        raise RuntimeError(f"a CI job failed: {run['html_url']}")
    args.out.mkdir(parents=True, exist_ok=True)
    report = {
        "producer": "scripts/record_ci_evidence.py",
        "branch": args.branch,
        "source_revision": run["head_sha"],
        "run_id": run["id"],
        "url": run["html_url"],
        "jobs": {},
    }
    for job in jobs:
        entry = {"conclusion": job["conclusion"], "url": job["html_url"]}
        if job["name"].startswith("test"):
            log = request(f"actions/jobs/{job['id']}/logs", token, raw=True)
            name = re.sub(r"[^a-z0-9]+", "-", job["name"].lower()).strip("-")
            (args.out / f"ci-{name}.txt").write_text(log, encoding="utf-8")
            entry.update(parse_job(log))
        report["jobs"][job["name"]] = entry
    (args.out / "ci.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {run['html_url']}")


if __name__ == "__main__":
    main()
