import pathlib
import shutil
import subprocess
import sys


def test_nested_basetemp_in_fresh_directory(tmp_path):
    hook = pathlib.Path(__file__).with_name("conftest.py")
    if hook.exists():
        shutil.copyfile(hook, tmp_path / "conftest.py")
    (tmp_path / "test_smoke.py").write_text(
        "def test_fixture(tmp_path):\n    assert tmp_path.is_dir()\n", encoding="utf-8"
    )
    assert not (tmp_path / "build").exists()
    result = subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "--basetemp=build/pytest"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
