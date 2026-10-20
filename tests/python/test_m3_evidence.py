import copy
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
from make_mutant import MUTATIONS, mutate, once
from summarize_m3 import replication_records
from verify_m3_mutations import failure, verify

ROOT = pathlib.Path(__file__).resolve().parents[2]


def record():
    failed = {
        "exit": 1,
        "output": (
            "[sim-failure] seed=7 replay=quorumkv_tests --seed=7\n"
            "[sim-trace-begin]\n1 event\n[sim-trace-end]\n"
            "Leader Completeness: elected leader changed committed prefix\n"
            "[  FAILED  ] ReplicationFuzz.CommittedHistorySurvivesSeededFaults\n"
        ),
    }
    return {
        "mutation": "commit_old_term",
        "runs": [
            copy.deepcopy(failed),
            copy.deepcopy(failed),
            copy.deepcopy(failed),
            {
                "exit": 0,
                "output": "[replication] first_seed=7 seeds=1 digest=123\n[  PASSED  ] 1 test.\n",
            },
            {
                "exit": 0,
                "output": "[mutation-detected] seed=7 invariant=Leader Completeness: lost\n"
                "[mutation-survey] first_seed=0 seeds=10000 detected=1\n"
                "[  PASSED  ] 1 test.\n",
            },
        ],
    }


def test_mutation_evidence_requires_failure_exact_replay_and_control():
    sample = record()
    assert verify(sample)["detected_seeds"] == 1
    sample["runs"][1]["output"] = sample["runs"][1]["output"].replace("1 event", "2 event")
    with pytest.raises(ValueError, match="replay"):
        verify(sample)
    sample = record()
    sample["runs"][3]["exit"] = 1
    with pytest.raises(ValueError, match="control"):
        verify(sample)


@pytest.mark.parametrize("code", [0, 2, 124, -11])
def test_passing_mutants_timeouts_and_process_crashes_are_not_safety_detections(code):
    run = record()["runs"][0]
    run["exit"] = code
    with pytest.raises(ValueError):
        failure(run)


@pytest.mark.parametrize("change", ["count", "unknown", "duplicate", "incomplete"])
def test_mutation_survey_rejects_misreported_or_unexpected_failures(change):
    sample = record()
    output = sample["runs"][4]["output"]
    if change == "count":
        output = output.replace("detected=1", "detected=2")
    elif change == "unknown":
        output = output.replace("Leader Completeness: lost", "out of range")
    elif change == "duplicate":
        output += "[mutation-detected] seed=7 invariant=Leader Completeness: lost\n"
        output = output.replace("detected=1", "detected=2")
    else:
        output = output.replace("[  PASSED  ] 1 test.", "")
    sample["runs"][4]["output"] = output
    with pytest.raises(ValueError):
        verify(sample)


def test_mutation_generation_is_explicit_and_rejects_source_drift():
    source = (ROOT / "src/raft/node.cpp").read_text()
    for name in MUTATIONS:
        assert mutate(source, name) != source
    with pytest.raises(ValueError):
        once("same same", "same", "changed")
    with pytest.raises(ValueError):
        mutate(source, "unknown")


def test_replication_summary_requires_checks_for_every_step_and_directed_phases():
    line = (
        "[replication] first_seed=0 seeds=10000 steps=20000 safety_checks=20000 "
        "log_checks=80000 application_checks=80000 proposals=10000 committed_commands=10000 "
        "figure8_runs=1250 ack_crash_runs=1250 crashes=1 sync_failures=1 candidate_crashes=1 "
        "append_reply_crashes=1 long_delay_packets=1 partitions=60000 dropped=1 duplicated=1 "
        "max_recovery_ticks=76 digest=123\n"
    )
    assert replication_records(line)[0]["seeds"] == 10000
    for corrupt in (
        line.replace("safety_checks=20000", "safety_checks=19999"),
        line.replace("application_checks=80000", "application_checks=79000"),
        line.replace("figure8_runs=1250", "figure8_runs=0"),
        line.replace("ack_crash_runs=1250", "ack_crash_runs=0"),
        line.replace("partitions=60000", "partitions=59999"),
        line.replace("max_recovery_ticks=76", "max_recovery_ticks=601"),
    ):
        with pytest.raises(ValueError):
            replication_records(corrupt)
