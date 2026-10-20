import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_watchdog_preserves_output_and_exit_code(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/test_watchdog.py"),
            "--stack-dir",
            str(tmp_path),
            sys.executable,
            "-c",
            "print('finished'); raise SystemExit(7)",
        ],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert result.returncode == 7
    assert result.stdout == "finished\n"


def test_watchdog_fails_exit_hang_and_attempts_stack_capture(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/test_watchdog.py"),
            "--exit-grace",
            "0.1",
            "--stack-dir",
            str(tmp_path),
            sys.executable,
            "-u",
            "-c",
            "import time; print('[  PASSED  ] 1 test.'); time.sleep(60)",
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert result.returncode == 124
    assert "[  PASSED  ] 1 test." in result.stdout
    assert "stalled; capturing" in result.stdout
    if sys.platform == "win32":
        assert list(tmp_path.glob("*.dmp"))
