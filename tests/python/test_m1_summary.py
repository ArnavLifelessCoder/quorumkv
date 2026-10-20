import json
import pathlib
import subprocess
import sys

import pytest

SCRIPTS = pathlib.Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import summarize_m1  # noqa: E402


def write_logs(directory, *, seed=17, count=7):
    fuzz = (
        f"[fuzz] first_seed={seed} seeds={count} round_trips={count * 6} "
        "probe_accepted=20 probe_rejected=36 corpus_digest=abcd"
    )
    suite = (
        "[  PASSED  ] 1 test.\n[  PASSED  ] 1 test.\n"
        f"{fuzz}\n100% tests passed, 0 tests failed out of 2\n"
    )
    for preset in ("debug", "asan"):
        (directory / f"{preset}-tests.txt").write_text(suite)
        (directory / f"{preset}-tools.json").write_text(
            json.dumps({"source_revision": "a" * 40, "source_status": ""})
        )
    replay_fuzz = (
        "[fuzz] first_seed=42 seeds=1 round_trips=6 "
        "probe_accepted=5 probe_rejected=3 corpus_digest=beef\n"
    )
    (directory / "replay.txt").write_text(
        replay_fuzz * 2
        + "Invalid CLI seed: exit=2\n"
        + "PASS: both replay fingerprints match; invalid CLI seed is rejected.\n"
    )


def test_derives_counts_from_discovered_test_logs(tmp_path):
    write_logs(tmp_path)
    result = summarize_m1.summarize(tmp_path)
    assert result["unique_seeds"] == 7
    assert result["seed_range"] == [17, 23]
    assert result["runs"]["debug"]["tests_passed"] == 2
    assert result["runs"]["debug"]["ctest_cases"] == 2
    assert result["runs"]["asan"]["round_trips"] == 42
    assert result["replay"]["seed"] == 42
    assert result["source_revision"] == "a" * 40
    write_logs(tmp_path, seed=101, count=3)
    assert summarize_m1.summarize(tmp_path)["seed_range"] == [101, 103]


@pytest.mark.parametrize(
    ("filename", "before", "after"),
    [
        ("asan-tests.txt", "100% tests passed", "50% tests passed"),
        ("asan-tests.txt", "0 tests failed", "1 tests failed"),
        ("asan-tests.txt", "out of 2", "out of 3"),
        ("asan-tests.txt", "corpus_digest=abcd", "corpus_digest=ffff"),
        ("debug-tests.txt", "[fuzz]", "[missing]"),
        ("debug-tests.txt", "seeds=7", "seeds=0"),
        ("replay.txt", "exit=2", "exit=0"),
        ("replay.txt", "PASS:", "FAIL:"),
        ("asan-tools.json", '"source_status": ""', '"source_status": " M src/rpc/codec.cpp"'),
        ("asan-tools.json", "a" * 40, "b" * 40),
    ],
)
def test_rejects_incomplete_failed_or_mismatched_evidence(tmp_path, filename, before, after):
    write_logs(tmp_path)
    path = tmp_path / filename
    path.write_text(path.read_text().replace(before, after))
    with pytest.raises(ValueError):
        summarize_m1.summarize(tmp_path)


def test_rejects_nonidentical_replay_attempts(tmp_path):
    write_logs(tmp_path)
    path = tmp_path / "replay.txt"
    path.write_text(path.read_text().replace("corpus_digest=beef", "corpus_digest=ffff", 1))
    with pytest.raises(ValueError):
        summarize_m1.summarize(tmp_path)


def test_cli_is_reproducible(tmp_path):
    write_logs(tmp_path)
    command = [sys.executable, str(SCRIPTS / "summarize_m1.py"), "--results-dir", str(tmp_path)]
    subprocess.run(command, check=True)
    first = (tmp_path / "summary.json").read_bytes()
    subprocess.run(command, check=True)
    assert (tmp_path / "summary.json").read_bytes() == first
