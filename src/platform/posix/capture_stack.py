"""Save all native thread backtraces from a stuck POSIX test process."""

import pathlib
import subprocess
import sys


def main() -> None:
    output = pathlib.Path(sys.argv[2]).with_suffix(".txt")
    result = subprocess.run(
        [
            "gdb",
            "-batch",
            "-ex",
            "set pagination off",
            "-ex",
            "thread apply all bt",
            "-p",
            sys.argv[1],
        ],
        capture_output=True,
        text=True,
        timeout=12,
        check=False,
    )
    output.write_text(result.stdout + result.stderr, encoding="utf-8")
    print(f"[watchdog] gdb exit={result.returncode}; stack capture: {output}")


if __name__ == "__main__":
    main()
