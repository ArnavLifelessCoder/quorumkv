"""Generate M2 evidence from complete successful logs and clean source metadata."""

import argparse
import hashlib
import json
import pathlib
import re

import summarize_m1
from verify_mutations import read_evidence
from verify_sim_replay import TRACE

FIELDS = (
    "first_seed seeds election_bound max_cold_ticks max_reelection_ticks max_recovery_ticks "
    "steps safety_checks delivered dropped duplicated partitions crashes restarts sync_failures"
    " p50_cold_ticks p99_cold_ticks p50_reelection_ticks p99_reelection_ticks"
    " vote_grant_crashes long_delay_packets stale_vote_deliveries"
).split()


def sim_records(text: str) -> list[dict]:
    records = []
    for line in re.findall(r"\[sim\] ([^\r\n]+)", text):
        pairs = [field.split("=") for field in line.split()]
        record = dict(pairs)
        if len(pairs) != len(FIELDS) + 1 or set(record) != {*FIELDS, "digest"}:
            raise ValueError("unexpected simulator summary fields")
        if not re.fullmatch("[0-9a-f]{1,16}", record["digest"]):
            raise ValueError("invalid simulator digest")
        record.update({field: int(record[field]) for field in FIELDS})
        count = record["seeds"]
        if (
            count <= 0
            or record["first_seed"] < 0
            or record["first_seed"] + count > 2**64
            or record["steps"] != record["safety_checks"]
            or record["steps"] <= 0
            or record["partitions"] != 6 * count
            or not 0 < record["max_cold_ticks"] <= record["election_bound"]
            or not 0 < record["max_reelection_ticks"] <= record["election_bound"]
            or not 90 < record["max_recovery_ticks"] <= record["election_bound"] + 90
            or not 0
            < record["p50_cold_ticks"]
            <= record["p99_cold_ticks"]
            <= record["max_cold_ticks"]
            or not (
                0
                < record["p50_reelection_ticks"]
                <= record["p99_reelection_ticks"]
                <= record["max_reelection_ticks"]
            )
            or any(record[field] < 0 for field in FIELDS)
            or record["dropped"] == 0
            or record["delivered"] == 0
        ):
            raise ValueError("simulator gate was not met")
        records.append(record)
    return records


def summarize(directory: pathlib.Path) -> dict:
    report = summarize_m1.summarize(directory)
    report["producer"] = "scripts/summarize_m2.py"
    simulations = {}
    for preset in ("debug", "asan"):
        records = sim_records((directory / f"{preset}-tests.txt").read_text(encoding="utf-8-sig"))
        if len(records) != 1 or records[0]["seeds"] < 10000:
            raise ValueError("expected one simulator campaign with at least 10000 seeds")
        if any(
            records[0][field] == 0
            for field in (
                "sync_failures",
                "vote_grant_crashes",
                "long_delay_packets",
                "stale_vote_deliveries",
            )
        ):
            raise ValueError("campaign did not exercise the strengthened fault model")
        simulations[preset] = records[0]
    if simulations["debug"] != simulations["asan"]:
        raise ValueError("debug and ASan simulator campaigns differ")
    replay = (directory / "sim-replay.txt").read_text(encoding="utf-8-sig")
    traces = TRACE.findall(replay)
    records = sim_records(replay)
    if (
        len(traces) != 3
        or len(records) != 3
        or not traces[0]
        or len(set(traces)) != 1
        or any(record != records[0] for record in records)
        or records[0]["seeds"] != 1
        or records[0]["first_seed"] != 42
        or "PASS: 3 simulator traces and summaries match exactly." not in replay
    ):
        raise ValueError("complete simulator replay did not match across builds")
    report["simulations"] = simulations
    mutations = {
        preset: read_evidence((directory / f"{preset}-mutations.jsonl").read_text(encoding="utf-8"))
        for preset in ("debug", "asan")
    }
    if mutations["debug"] != mutations["asan"]:
        raise ValueError("debug and ASan mutation evidence differ")
    report["mutations"] = mutations
    report["simulator_replay"] = {
        "seed": records[0]["first_seed"],
        "runs": len(traces),
        "trace_sha256": hashlib.sha256(traces[0].encode()).hexdigest(),
        "summary": records[0],
    }
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent / "results" / "m2",
    )
    args = parser.parse_args()
    report = summarize(args.results_dir)
    output = args.results_dir / "summary.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {output} from verified codec and simulator evidence.")


if __name__ == "__main__":
    main()
