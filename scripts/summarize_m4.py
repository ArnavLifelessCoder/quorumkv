"""Generate M4 evidence only from complete successful logs, replays, and negative controls."""

import argparse
import hashlib
import json
import pathlib
import re

import summarize_m3
from verify_sim_replay import TRACE

FIELDS = (
    "first_seed seeds operations acknowledged retries timeouts not_leader logged_reads "
    "reads_served reads_failed applied_commands crashes partitions steps safety_checks "
    "max_quiesce_ticks"
).split()
NEGATIVE = re.compile(r"^\[kv-negative\] mode=(\w+) first_seed=(\d+) error=(.+)$", re.MULTILINE)
EXPECTED_ERRORS = {
    "unsafe_reads": ("Stale read", "Read of a state that never committed"),
    "no_dedupe": ("Exactly-once",),
}
NEGATIVE_LIMIT = 1000
PARTITIONS_PER_SEED = 8


def kv_records(text: str) -> list[dict]:
    records = []
    for line in re.findall(r"\[kv\] ([^\r\n]+)", text):
        pairs = [field.split("=", 1) for field in line.split()]
        record = dict(pairs)
        if len(pairs) != len(FIELDS) + 1 or set(record) != {*FIELDS, "digest"}:
            raise ValueError("unexpected KV summary fields")
        if not re.fullmatch("[0-9a-f]{1,16}", record["digest"]):
            raise ValueError("invalid KV digest")
        record.update({field: int(record[field]) for field in FIELDS})
        count = record["seeds"]
        if (
            count <= 0
            or record["steps"] <= 0
            or record["safety_checks"] != record["steps"]
            or record["acknowledged"] != record["operations"]
            or record["operations"] < count
            or record["partitions"] != PARTITIONS_PER_SEED * count
            or not 0 < record["max_quiesce_ticks"] <= 1500
        ):
            raise ValueError("KV safety, completion, or fault gate was not met")
        records.append(record)
    return records


def negative_records(text: str) -> dict:
    found = {}
    for mode, seed, error in NEGATIVE.findall(text):
        seed = int(seed)
        if mode in found or mode not in EXPECTED_ERRORS:
            raise ValueError(f"unexpected or duplicate negative control {mode}")
        if seed >= NEGATIVE_LIMIT or not error.strip().startswith(EXPECTED_ERRORS[mode]):
            raise ValueError(f"checker did not flag deliberately broken mode {mode}")
        found[mode] = {"first_seed": seed, "error": error.strip()}
    if set(found) != set(EXPECTED_ERRORS):
        raise ValueError("every deliberately broken mode must be flagged")
    return found


def summarize(directory: pathlib.Path) -> dict:
    report = summarize_m3.summarize(directory)
    report["producer"] = "scripts/summarize_m4.py"
    campaigns = {}
    negatives = {}
    for preset in ("debug", "asan"):
        text = (directory / f"{preset}-tests.txt").read_text(encoding="utf-8-sig")
        records = kv_records(text)
        if len(records) != 1 or records[0]["seeds"] != 10000:
            raise ValueError("expected a complete 10000-seed KV campaign")
        for field in ("retries", "timeouts", "logged_reads", "reads_served", "crashes"):
            if records[0][field] == 0:
                raise ValueError(f"KV campaign did not exercise {field}")
        campaigns[preset] = records[0]
        negatives[preset] = negative_records(text)
    if campaigns["debug"] != campaigns["asan"] or negatives["debug"] != negatives["asan"]:
        raise ValueError("debug and ASan KV campaigns differ")
    report["kv"] = campaigns
    report["kv_negative_controls"] = negatives["debug"]
    replay = (directory / "kv-replay.txt").read_text(encoding="utf-8-sig")
    traces, records = TRACE.findall(replay), kv_records(replay)
    if (
        len(traces) != 3
        or len(records) != 3
        or not traces[0]
        or len(set(traces)) != 1
        or any(record != records[0] for record in records)
        or records[0]["seeds"] != 1
        or "PASS: 3 KV traces and summaries match exactly." not in replay
    ):
        raise ValueError("complete KV replay did not match across builds")
    report["kv_replay"] = {
        "seed": records[0]["first_seed"],
        "runs": 3,
        "trace_sha256": hashlib.sha256(traces[0].encode()).hexdigest(),
        "summary": records[0],
    }
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1] / "results/m4",
    )
    args = parser.parse_args()
    report = summarize(args.results_dir)
    output = args.results_dir / "summary.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {output} from complete verified evidence.")


if __name__ == "__main__":
    main()
