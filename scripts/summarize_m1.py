"""Generate M1's summary solely from successful test/replay logs and clean-source metadata."""

import argparse
import json
import pathlib
import re

FUZZ = re.compile(
    r"\[fuzz\] first_seed=(\d+) seeds=(\d+) round_trips=(\d+) "
    r"probe_accepted=(\d+) probe_rejected=(\d+) corpus_digest=([0-9a-f]+)"
)


def fuzz_records(text: str) -> list[dict]:
    records = []
    for match in FUZZ.finditer(text):
        keys = ("first_seed", "seeds", "round_trips", "probe_accepted", "probe_rejected")
        record = dict(zip(keys, map(int, match.groups()[:5]), strict=True))
        if not record["seeds"] or record["first_seed"] + record["seeds"] > 2**64:
            raise ValueError("invalid seed range")
        record["corpus_digest"] = match.group(6)
        records.append(record)
    return records


def read_run(path: pathlib.Path) -> dict:
    text = path.read_text(encoding="utf-8-sig")
    records = fuzz_records(text)
    footer = re.findall(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", text)
    passed = [int(value) for value in re.findall(r"\[  PASSED  \] (\d+) tests?\.", text)]
    if len(records) != 1 or len(footer) != 1 or not passed:
        raise ValueError(f"{path.name}: missing or duplicate test summaries")
    percent, failed, cases = map(int, footer[0])
    if percent != 100 or failed != 0 or cases != sum(passed):
        raise ValueError(f"{path.name}: unsuccessful or incomplete discovered test suite")
    return {"tests_passed": sum(passed), "ctest_cases": cases, **records[0]}


def summarize(directory: pathlib.Path) -> dict:
    runs = {preset: read_run(directory / f"{preset}-tests.txt") for preset in ("debug", "asan")}
    if runs["debug"] != runs["asan"]:
        raise ValueError("debug and ASan results do not match")
    revisions = []
    for preset in runs:
        metadata = json.loads((directory / f"{preset}-tools.json").read_text(encoding="utf-8-sig"))
        revision = metadata.get("source_revision", "")
        if not re.fullmatch(r"[0-9a-f]{40}", revision) or metadata.get("source_status") != "":
            raise ValueError(f"{preset}: evidence must identify a clean source commit")
        revisions.append(revision)
    if revisions[0] != revisions[1]:
        raise ValueError("debug and ASan used different source commits")

    replay_text = (directory / "replay.txt").read_text(encoding="utf-8-sig")
    replays = fuzz_records(replay_text)
    exits = re.findall(r"Invalid CLI seed: exit=(\d+)", replay_text)
    if (
        len(replays) != 2
        or replays[0] != replays[1]
        or replays[0]["seeds"] != 1
        or exits != ["2"]
        or "PASS: both replay fingerprints match; invalid CLI seed is rejected." not in replay_text
    ):
        raise ValueError("replay or invalid-seed verification did not pass")
    first = runs["debug"]["first_seed"]
    count = runs["debug"]["seeds"]
    return {
        "producer": "scripts/summarize_m1.py",
        "source_revision": revisions[0],
        "source_status": "",
        "unique_seeds": count,
        "seed_range": [first, first + count - 1],
        "runs": runs,
        "replay": {
            "seed": replays[0]["first_seed"],
            "runs": len(replays),
            "matching_fingerprints": replays[0] == replays[1],
            "corpus_digest": replays[0]["corpus_digest"],
            "invalid_seed_exit_code": int(exits[0]),
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent / "results" / "m1",
    )
    arguments = parser.parse_args()
    report = summarize(arguments.results_dir)
    output = arguments.results_dir / "summary.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {output} from debug, ASan, replay logs, and source metadata.")


if __name__ == "__main__":
    main()
