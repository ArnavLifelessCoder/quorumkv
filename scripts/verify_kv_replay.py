"""Compare complete KV workload traces across repeated runs and optional sanitizer builds."""

import argparse
import pathlib
import re
import subprocess
import sys

from verify_sim_replay import TRACE

SEED = 42


def signature(text: str) -> tuple[str, str]:
    traces = TRACE.findall(text)
    summaries = re.findall(r"^\[kv\] .+$", text, re.MULTILINE)
    if len(traces) != 1 or not traces[0] or len(summaries) != 1:
        raise ValueError("expected one complete KV trace and summary")
    return traces[0], summaries[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=pathlib.Path)
    parser.add_argument("--compare-executable", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    executables = [args.executable, args.executable]
    if args.compare_executable:
        executables.append(args.compare_executable)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    signatures = []
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        for executable in executables:
            result = subprocess.run(
                [
                    sys.executable,
                    str(pathlib.Path(__file__).with_name("test_watchdog.py")),
                    str(executable.resolve(strict=True)),
                    f"--seed={SEED}",
                    "--gtest_filter=KvFuzz.Exactly*",
                ],
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
            output.write(f"Replay: {executable}\n{result.stdout}{result.stderr}\n")
            if result.returncode:
                raise ValueError("KV replay failed")
            signatures.append(signature(result.stdout))
        if any(item != signatures[0] for item in signatures):
            raise ValueError("KV traces or outcomes differ")
        output.write(f"PASS: {len(signatures)} KV traces and summaries match exactly.\n")
    print(signatures[0][1])
    print(f"Verified KV replay: {args.output}")


if __name__ == "__main__":
    main()
