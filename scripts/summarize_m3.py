"""Generate M3 evidence only from successful complete logs, replay, and mutation surveys."""

import argparse
import hashlib
import json
import pathlib
import re

import summarize_m1
from summarize_m2 import sim_records
from verify_m3_mutations import read_evidence
from verify_sim_replay import TRACE

FIELDS = (
    "first_seed seeds steps safety_checks log_checks application_checks proposals "
    "committed_commands figure8_runs ack_crash_runs crashes sync_failures candidate_crashes "
    "append_reply_crashes long_delay_packets "
    "partitions dropped duplicated max_recovery_ticks"
).split()


def replication_records(text: str) -> list[dict]:
    records = []
    for line in re.findall(r"\[replication\] ([^\r\n]+)", text):
        pairs = [field.split("=") for field in line.split()]
        record = dict(pairs)
        if len(pairs) != len(FIELDS) + 1 or set(record) != {*FIELDS, "digest"}:
            raise ValueError("unexpected replication summary fields")
        if not re.fullmatch("[0-9a-f]{1,16}", record["digest"]):
            raise ValueError("invalid replication digest")
        record.update({field: int(record[field]) for field in FIELDS})
        count, first, steps = record["seeds"], record["first_seed"], record["steps"]
        if (
            count <= 0
            or first < 0
            or first + count > 2**64
            or steps <= 0
            or any(record[field] < 0 for field in FIELDS)
            or record["safety_checks"] != steps
            or not 3 * steps <= record["log_checks"] <= 5 * steps
            or record["application_checks"] != record["log_checks"]
            or record["partitions"] != 6 * count
            or record["committed_commands"] < count
            or record["figure8_runs"] != (first + count) // 8 - first // 8
            or record["ack_crash_runs"] != (first + count + 1) // 8 - (first + 1) // 8
            or not 71 <= record["max_recovery_ticks"] <= 600
        ):
            raise ValueError("replication safety or recovery gate was not met")
        records.append(record)
    return records


def summarize(directory: pathlib.Path) -> dict:
    report = summarize_m1.summarize(directory)
    report["producer"] = "scripts/summarize_m3.py"
    campaigns = {}
    elections = {}
    for preset in ("debug", "asan"):
        text = (directory / f"{preset}-tests.txt").read_text(encoding="utf-8-sig")
        records = replication_records(text)
        if len(records) != 1 or records[0]["seeds"] != 10000:
            raise ValueError("expected a complete 10000-seed replication campaign")
        for field in (
            "sync_failures",
            "candidate_crashes",
            "append_reply_crashes",
            "long_delay_packets",
            "dropped",
            "duplicated",
        ):
            if records[0][field] == 0:
                raise ValueError(f"campaign did not exercise {field}")
        campaigns[preset] = records[0]
        election = sim_records(text)
        if len(election) != 1 or election[0]["seeds"] != 10000:
            raise ValueError("expected complete election regression campaign")
        elections[preset] = election[0]
    if campaigns["debug"] != campaigns["asan"] or elections["debug"] != elections["asan"]:
        raise ValueError("debug and ASan campaigns differ")
    report["replication"] = campaigns
    report["elections"] = elections
    replay = (directory / "replication-replay.txt").read_text(encoding="utf-8-sig")
    traces, records = TRACE.findall(replay), replication_records(replay)
    if (
        len(traces) != 3
        or len(records) != 3
        or not traces[0]
        or len(set(traces)) != 1
        or any(record != records[0] for record in records)
        or records[0]["first_seed"] != 47
        or records[0]["seeds"] != 1
        or "PASS: 3 replication traces and summaries match exactly." not in replay
    ):
        raise ValueError("complete replication replay did not match across builds")
    report["replication_replay"] = {
        "seed": 47,
        "runs": 3,
        "trace_sha256": hashlib.sha256(traces[0].encode()).hexdigest(),
        "summary": records[0],
    }
    mutations = {
        preset: read_evidence((directory / f"{preset}-mutations.jsonl").read_text(encoding="utf-8"))
        for preset in ("debug", "asan")
    }
    for name, debug in mutations["debug"].items():
        if debug.get("survey_seeds") != 10000:
            raise ValueError("every mutation requires a complete detection survey")
        common = {
            key: value
            for key, value in debug.items()
            if key not in ("survey_seeds", "detected_seeds")
        }
        if common != mutations["asan"][name]:
            raise ValueError("mutation replay differs across builds")
    report["mutations"] = mutations
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1] / "results/m3",
    )
    args = parser.parse_args()
    report = summarize(args.results_dir)
    output = args.results_dir / "summary.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {output} from complete verified evidence.")


if __name__ == "__main__":
    main()
