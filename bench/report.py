"""Render results/m8/benchmarks.json as the README results table and PNG plots.

    python bench/report.py [--results results/m8]

Prints a Markdown table to stdout and writes PNG plots next to the JSON. Every number it
prints is read from benchmarks.json, which bench/run_benchmarks.py produced.
"""

from __future__ import annotations

import argparse
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]

THROUGHPUT_ROWS = [
    ("write_3_nodes", "Write, 3 nodes, 100 B values"),
    ("write_5_nodes", "Write, 5 nodes, 100 B values"),
    ("batching_on_pipelining_on", "Write, batching and pipelining on"),
    ("pipelining_off", "Write, pipelining off"),
    ("batching_off", "Write, batching off"),
    ("batching_and_pipelining_off", "Write, batching and pipelining off"),
    ("read_index", "Read through ReadIndex"),
    ("read_through_log", "Read through the log"),
    ("value_100_b", "Write, 100 B values"),
    ("value_1_kb", "Write, 1 KB values"),
    ("value_10_kb", "Write, 10 KB values"),
]


def table(report: dict) -> str:
    lines = [
        "| Benchmark | ops/sec (median, min to max) | p50 latency | p99 latency |",
        "| --- | ---: | ---: | ---: |",
    ]
    for key, label in THROUGHPUT_ROWS:
        if key not in report:
            continue
        row = report[key]
        lines.append(
            f"| {label} | {row['ops_per_sec_median']:,.0f} "
            f"({row['ops_per_sec_min']:,.0f} to {row['ops_per_sec_max']:,.0f}) "
            f"| {row['p50_us_median'] / 1000:.1f} ms | {row['p99_us_median'] / 1000:.1f} ms |"
        )
    if "failover" in report:
        row = report["failover"]
        if "election_ms" in row:
            lines.append(
                f"| New leader elected after SIGKILL of the leader, {len(row['election_ms'])} "
                f"trials | | {row['election_median_ms']:.0f} ms median "
                f"| {row['election_p99_ms']:.0f} ms p99 |"
            )
        lines.append(
            f"| First client write after SIGKILL of the leader, {row['measured']} trials "
            f"| | {row['median_ms']:.0f} ms median | {row['p99_ms']:.0f} ms p99 |"
        )
    if "snapshot_catch_up" in report:
        row = report["snapshot_catch_up"]
        behind = min(run["entries_behind"] for run in row["runs"])
        lines.append(
            f"| Follower catch-up from at least {behind:,} entries behind, "
            f"{len(row['runs'])} runs | | {row['median_s']:.1f} s median "
            f"| {row['min_s']:.1f} to {row['max_s']:.1f} s |"
        )
    return "\n".join(lines)


def plots(report: dict, directory: pathlib.Path) -> list[pathlib.Path]:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return []
    written = []
    rows = [(label, report[key]) for key, label in THROUGHPUT_ROWS if key in report]
    if rows:
        figure, axis = plt.subplots(figsize=(9, 5))
        labels = [label for label, _ in rows]
        medians = [row["ops_per_sec_median"] for _, row in rows]
        low = [row["ops_per_sec_median"] - row["ops_per_sec_min"] for _, row in rows]
        high = [row["ops_per_sec_max"] - row["ops_per_sec_median"] for _, row in rows]
        axis.barh(labels, medians, xerr=[low, high], color="#4c72b0")
        axis.invert_yaxis()
        axis.set_xlabel("operations per second (median, bars show min to max)")
        axis.set_title("quorumkv throughput, localhost cluster")
        figure.tight_layout()
        path = directory / "throughput.png"
        figure.savefig(path, dpi=120)
        plt.close(figure)
        written.append(path)
    if "failover" in report:
        figure, axis = plt.subplots(figsize=(7, 4))
        axis.hist(report["failover"]["failover_ms"], bins=20, color="#dd8452")
        axis.set_xlabel("milliseconds from SIGKILL of the leader to the next successful write")
        axis.set_ylabel("trials")
        axis.set_title("Failover time")
        figure.tight_layout()
        path = directory / "failover.png"
        figure.savefig(path, dpi=120)
        plt.close(figure)
        written.append(path)
    return written


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=pathlib.Path, default=ROOT / "results" / "m8")
    args = parser.parse_args()
    report = json.loads((args.results / "benchmarks.json").read_text())
    print(table(report))
    for path in plots(report, args.results):
        print(f"\nWrote {path}")


if __name__ == "__main__":
    main()
