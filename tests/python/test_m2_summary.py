import pathlib
import sys

import pytest
from test_m1_summary import write_logs

SCRIPTS = pathlib.Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import summarize_m2  # noqa: E402
from verify_sim_replay import signature  # noqa: E402


def simulation(*, seed=0, count=10000):
    return (
        f"[sim] first_seed={seed} seeds={count} election_bound=300 max_cold_ticks=50 "
        "max_reelection_ticks=70 max_recovery_ticks=41 steps=200 safety_checks=200 "
        f"delivered=100 dropped=5 duplicated=3 partitions={6 * count} crashes=4 "
        "restarts=7 sync_failures=1 digest=abc\n"
    )


def logs(directory):
    write_logs(directory, count=10000)
    for preset in ("debug", "asan"):
        path = directory / f"{preset}-tests.txt"
        path.write_text(path.read_text() + simulation())
    replay = "[sim-trace-begin]\n1 0 restart 1 abcd\n[sim-trace-end]\n"
    (directory / "sim-replay.txt").write_text(
        (replay + simulation(seed=42, count=1)) * 3
        + "PASS: 3 simulator traces and summaries match exactly.\n"
    )


def test_derives_simulator_summary_and_trace_hash(tmp_path):
    logs(tmp_path)
    report = summarize_m2.summarize(tmp_path)
    assert report["producer"] == "scripts/summarize_m2.py"
    assert report["simulations"]["debug"]["seeds"] == 10000
    assert report["simulations"]["asan"]["safety_checks"] == 200
    assert report["simulator_replay"]["runs"] == 3
    assert len(report["simulator_replay"]["trace_sha256"]) == 64


@pytest.mark.parametrize(
    ("file", "before", "after"),
    [
        ("asan-tests.txt", "digest=abc", "digest=def"),
        ("debug-tests.txt", "safety_checks=200", "safety_checks=199"),
        ("debug-tests.txt", "max_cold_ticks=50", "max_cold_ticks=301"),
        ("debug-tests.txt", "max_reelection_ticks=70", "max_reelection_ticks=301"),
        ("debug-tests.txt", "max_recovery_ticks=41", "max_recovery_ticks=341"),
        ("debug-tests.txt", "seeds=10000", "seeds=9999"),
        ("debug-tests.txt", "partitions=60000", "partitions=0"),
        ("debug-tests.txt", "dropped=5", "dropped=0"),
        ("debug-tests.txt", "[sim]", "[missing]"),
        ("sim-replay.txt", "1 0 restart 1 abcd", "2 0 restart 1 abcd"),
        ("sim-replay.txt", "digest=abc", "digest=def"),
        ("sim-replay.txt", "PASS:", "FAIL:"),
    ],
)
def test_rejects_incomplete_or_inconsistent_simulation(tmp_path, file, before, after):
    logs(tmp_path)
    path = tmp_path / file
    path.write_text(path.read_text().replace(before, after, 1))
    with pytest.raises(ValueError):
        summarize_m2.summarize(tmp_path)


def test_replay_signature_requires_trace_and_summary():
    with pytest.raises(ValueError):
        signature(simulation())
    with pytest.raises(ValueError):
        signature("[sim-trace-begin]\n[sim-trace-end]\n" + simulation())
