"""Save a successful seven-job CI run and its full TSan log for an exact source revision."""

import argparse
import json
import pathlib
import subprocess
import time
import urllib.request

REPOSITORY = "ArnavLifelessCoder/quorumkv"


def credential() -> str:
    result = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        capture_output=True,
        text=True,
        check=True,
    )
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    return fields["password"]


def request(path: str, token: str, raw: bool = False):
    url = f"https://api.github.com/repos/{REPOSITORY}/{path}"
    query = urllib.request.Request(
        url, headers={"Accept": "application/vnd.github+json", "User-Agent": "quorumkv-ci-evidence"}
    )
    # Do not forward credentials to the signed artifact/log redirect destination.
    query.add_unredirected_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(query, timeout=60) as response:
        data = response.read()
    return data.decode("utf-8-sig") if raw else json.loads(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--branch", default="m3")
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--wait-seconds", type=int, default=0)
    args = parser.parse_args()
    token = credential()
    deadline = time.monotonic() + args.wait_seconds
    previous = None
    while True:
        runs = request(
            f"actions/runs?branch={args.branch}&head_sha={args.revision}&per_page=30", token
        )
        matches = [
            run
            for run in runs["workflow_runs"]
            if run["head_sha"] == args.revision
            and run["name"] == "CI"
            and run["head_branch"] == args.branch
        ]
        run = matches[0] if matches else None
        jobs = request(f"actions/runs/{run['id']}/jobs?per_page=100", token)["jobs"] if run else []
        status = [(job["name"], job["status"], job["conclusion"]) for job in jobs]
        if status != previous:
            print(json.dumps({"revision": args.revision, "jobs": status}), flush=True)
            previous = status
        if run and run["status"] == "completed":
            if (
                run["conclusion"] != "success"
                or len(jobs) != 7
                or any(job["conclusion"] != "success" for job in jobs)
            ):
                raise RuntimeError(f"CI gate failed: {run['html_url']}")
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("CI has not completed for the requested source revision")
        time.sleep(30)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    tsan = [job for job in jobs if "tsan" in job["name"]]
    if len(tsan) != 1:
        raise ValueError("expected exactly one TSan job")
    log = request(f"actions/jobs/{tsan[0]['id']}/logs", token, raw=True)
    (args.output_dir / "ci-tsan.txt").write_text(log, encoding="utf-8")
    report = {
        "producer": "scripts/fetch_ci.py",
        "source_revision": args.revision,
        "run_id": run["id"],
        "url": run["html_url"],
        "conclusion": run["conclusion"],
        "jobs": [
            {key: job[key] for key in ("id", "name", "conclusion", "html_url")} for job in jobs
        ],
    }
    (args.output_dir / "ci.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Saved all seven successful jobs and full TSan output: {run['html_url']}", flush=True)


if __name__ == "__main__":
    main()
