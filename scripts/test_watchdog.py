"""Run one CTest process; capture a stuck process before failing and terminating it."""

import argparse
import pathlib
import queue
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def capture(pid: int, destination: pathlib.Path) -> None:
    platform = "windows" if sys.platform == "win32" else "posix"
    command = [
        sys.executable,
        str(ROOT / "src" / "platform" / platform / "capture_stack.py"),
        str(pid),
        str(destination),
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=15, check=False)
        print(result.stdout + result.stderr, end="", flush=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"[watchdog] stack capture failed: {error}", flush=True)


def run(command: list[str], timeout: float, grace: float, directory: pathlib.Path) -> int:
    started = time.monotonic()
    passed_at = None
    lines: queue.Queue[str] = queue.Queue()
    with subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    ) as process:

        def read() -> None:
            assert process.stdout is not None
            for line in process.stdout:
                lines.put(line)

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        while True:
            try:
                line = lines.get(timeout=0.05)
                print(line, end="", flush=True)
                if "[  PASSED  ]" in line:
                    passed_at = time.monotonic()
            except queue.Empty:
                pass
            code = process.poll()
            if code is not None:
                reader.join(timeout=1)
                while not lines.empty():
                    print(lines.get_nowait(), end="", flush=True)
                return code
            now = time.monotonic()
            if now - started >= timeout or (passed_at is not None and now - passed_at >= grace):
                directory.mkdir(parents=True, exist_ok=True)
                destination = directory / f"process-{process.pid}-{time.time_ns()}"
                print(
                    f"[watchdog] process {process.pid} stalled; capturing {destination}", flush=True
                )
                capture(process.pid, destination)
                process.kill()
                process.wait(timeout=10)
                return 124


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float)
    parser.add_argument("--exit-grace", type=float, default=10)
    parser.add_argument("--stack-dir", type=pathlib.Path, default=ROOT / "build" / "stacks")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error("test command required")
    timeout = args.timeout or (5400 if any("Fuzz" in arg for arg in args.command) else 60)
    raise SystemExit(run(args.command, timeout, args.exit_grace, args.stack_dir))


if __name__ == "__main__":
    main()
