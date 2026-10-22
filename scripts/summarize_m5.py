"""Generate M5 evidence only from complete successful logs, controls, and the long run."""

import argparse
import json
import pathlib
import re

import summarize_m4

SNAPSHOT_FIELDS = (
    "first_seed seeds snapshots_taken snapshot_installs snapshot_chunks restore_checks "
    "max_retained_entries"
).split()
LONGRUN_FIELDS = (
    "ticks threshold operations applied_commands snapshots_taken max_retained_entries"
).split()
NEGATIVE = re.compile(
    r"^\[kv-snapshot-negative\] mode=(\w+) first_seed=(\d+) error=(.+)$", re.MULTILINE
)
NEGATIVE_LIMIT = 1000
NEGATIVE_ERRORS = ("Exactly-once", "State Machine Safety")


def fields(line: str, names: list[str], label: str) -> dict:
    pairs = [field.split("=", 1) for field in line.split()]
    record = dict(pairs)
    if len(pairs) != len(names) or set(record) != set(names):
        raise ValueError(f"unexpected {label} fields")
    return {name: int(record[name]) for name in names}


def snapshot_records(text: str) -> list[dict]:
    records = []
    for line in re.findall(r"^\[kv-snapshot\] ([^\r\n]+)", text, re.MULTILINE):
        record = fields(line, SNAPSHOT_FIELDS, "snapshot campaign")
        if (
            record["seeds"] <= 0
            or record["snapshots_taken"] <= 0
            or record["snapshot_installs"] <= 0
            or record["snapshot_chunks"] <= record["snapshot_installs"]
            or record["restore_checks"] <= 0
        ):
            raise ValueError("snapshot campaign did not exercise compaction and InstallSnapshot")
        records.append(record)
    return records


def longrun_record(text: str) -> dict:
    lines = re.findall(r"^\[kv-longrun\] ([^\r\n]+)", text, re.MULTILINE)
    if len(lines) != 1:
        raise ValueError("expected exactly one long-running workload summary")
    record = fields(lines[0], LONGRUN_FIELDS, "long run")
    # The bound is the threshold plus at most one tick of new entries above it.
    if (
        record["operations"] <= 1000
        or record["snapshots_taken"] <= 50
        or record["max_retained_entries"] > 2 * record["threshold"]
    ):
        raise ValueError("long-running workload did not keep the log bounded")
    return record


def negative_record(text: str) -> dict:
    found = NEGATIVE.findall(text)
    if len(found) != 1:
        raise ValueError("expected exactly one snapshot negative control")
    mode, seed, error = found[0]
    seed, error = int(seed), error.strip()
    if (
        mode != "sessionless_snapshot"
        or seed >= NEGATIVE_LIMIT
        or not (error.startswith(NEGATIVE_ERRORS) or "snapshot" in error)
    ):
        raise ValueError("checker did not flag snapshots that drop sessions")
    return {"mode": mode, "first_seed": seed, "error": error}


def summarize(directory: pathlib.Path) -> dict:
    report = summarize_m4.summarize(directory)
    report["producer"] = "scripts/summarize_m5.py"
    campaigns, longruns, negatives = {}, {}, {}
    for preset in ("debug", "asan"):
        text = (directory / f"{preset}-tests.txt").read_text(encoding="utf-8-sig")
        records = snapshot_records(text)
        if len(records) != 1 or records[0]["seeds"] != 10000:
            raise ValueError("expected a complete 10000-seed snapshot campaign")
        campaigns[preset] = records[0]
        longruns[preset] = longrun_record(text)
        negatives[preset] = negative_record(text)
    for name, values in (("snapshot", campaigns), ("long run", longruns), ("control", negatives)):
        if values["debug"] != values["asan"]:
            raise ValueError(f"debug and ASan {name} results differ")
    report["snapshots"] = campaigns
    report["log_bound"] = longruns["debug"]
    report["snapshot_negative_control"] = negatives["debug"]
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1] / "results/m5",
    )
    args = parser.parse_args()
    report = summarize(args.results_dir)
    output = args.results_dir / "summary.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {output} from complete verified evidence.")


if __name__ == "__main__":
    main()
