import json
import pathlib
import sys

import pytest
from test_m1_summary import write_logs

SCRIPTS = pathlib.Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import summarize_m2  # noqa: E402
from verify_mutations import INVARIANT, MUTATIONS, failure_signature, verify_runs  # noqa: E402
from verify_sim_replay import signature  # noqa: E402


def simulation(*, seed=0, count=10000):
    return (
        f"[sim] first_seed={seed} seeds={count} election_bound=300 max_cold_ticks=50 "
        "max_reelection_ticks=70 max_recovery_ticks=91 steps=200 safety_checks=200 "
        f"delivered=100 dropped=5 duplicated=3 partitions={6 * count} crashes=4 "
        "restarts=7 sync_failures=1 p50_cold_ticks=20 p99_cold_ticks=40 "
        "p50_reelection_ticks=30 p99_reelection_ticks=60 vote_grant_crashes=2 "
        "long_delay_packets=10 stale_vote_deliveries=3 digest=abc\n"
    )


def mutation_runs():
    trace = "[sim-trace-begin]\n1 0 restart 1 abcd\n[sim-trace-end]\n"
    failed = {
        "exit": 1,
        "output": trace
        + f"[sim-failure] seed=7 replay=quorumkv_tests --seed=7\n{INVARIANT}\n"
        + "[  FAILED  ] ElectionFuzz.SafetyAndRecoveryAcrossSeededFaultSchedules\n",
    }
    return [
        dict(failed),
        dict(failed),
        dict(failed),
        {
            "exit": 0,
            "output": trace + simulation(seed=7, count=1) + "[  PASSED  ] 1 test.\n",
        },
    ]


def logs(directory):
    write_logs(directory, count=10000)
    for preset in ("debug", "asan"):
        path = directory / f"{preset}-tests.txt"
        path.write_text(path.read_text() + simulation())
        (directory / f"{preset}-mutations.jsonl").write_text(
            "\n".join(
                json.dumps({"mutation": mutation, "runs": mutation_runs()})
                for mutation in MUTATIONS
            )
            + "\n"
        )
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
    assert report["mutations"]["debug"]["reply_before_persist"]["seed"] == 7


@pytest.mark.parametrize(
    ("file", "before", "after"),
    [
        ("asan-tests.txt", "digest=abc", "digest=def"),
        ("debug-tests.txt", "safety_checks=200", "safety_checks=199"),
        ("debug-tests.txt", "max_cold_ticks=50", "max_cold_ticks=301"),
        ("debug-tests.txt", "max_reelection_ticks=70", "max_reelection_ticks=301"),
        ("debug-tests.txt", "max_recovery_ticks=91", "max_recovery_ticks=391"),
        ("debug-tests.txt", "p50_cold_ticks=20", "p50_cold_ticks=45"),
        ("debug-tests.txt", "long_delay_packets=10", "long_delay_packets=0"),
        ("debug-tests.txt", "stale_vote_deliveries=3", "stale_vote_deliveries=0"),
        ("debug-tests.txt", "vote_grant_crashes=2", "vote_grant_crashes=0"),
        ("debug-mutations.jsonl", INVARIANT, "leader election exceeded 300 ticks"),
        ("asan-mutations.jsonl", "restart 1 abcd", "restart 1 abce"),
        ("asan-mutations.jsonl", '"exit": 1', '"exit": 0'),
        ("asan-mutations.jsonl", "count_stale_votes", "wrong_mutation"),
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


@pytest.mark.parametrize("code", [0, 2, -11])
def test_mutation_rejects_success_cli_errors_and_process_crashes(code):
    with pytest.raises(ValueError):
        failure_signature(code, mutation_runs()[0]["output"])


@pytest.mark.parametrize("index", [0, 1, 2, 3])
def test_mutation_requires_all_search_replays_and_control(index):
    runs = mutation_runs()
    runs[index]["output"] = ""
    with pytest.raises(ValueError):
        verify_runs(runs)


def test_mutation_rejects_a_failing_normal_node():
    runs = mutation_runs()
    runs[3]["exit"] = 1
    with pytest.raises(ValueError):
        verify_runs(runs)
