"""Compare complete event traces from repeated simulator runs and optional sanitizer builds."""

import argparse
import pathlib
import re
import subprocess

TRACE = re.compile(r"\[sim-trace-begin\]\n(.*?)\[sim-trace-end\]", re.DOTALL)


def signature(text: str) -> tuple[str, str]:
    traces = TRACE.findall(text)
    summaries = re.findall(r"^\[sim\] .+$", text, re.MULTILINE)
    if len(traces) != 1 or not traces[0] or len(summaries) != 1:
        raise ValueError("expected one nonempty simulator trace and summary")
    return traces[0], summaries[0]


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
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
    with args.output.open("w", encoding="utf-8", newline="\n") as log:
        for attempt, executable in enumerate(executables, start=1):
            result = subprocess.run(
                [
                    str(executable.resolve(strict=True)),
                    "--seed=42",
                    "--gtest_filter=ElectionFuzz.*",
                ],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
            log.write(f"Replay attempt {attempt}: {executable}\n{result.stdout}{result.stderr}\n")
            if result.returncode:
                raise RuntimeError(f"simulator replay failed with exit {result.returncode}")
            signatures.append(signature(result.stdout))
        if any(item != signatures[0] for item in signatures):
            raise RuntimeError("simulator event traces or outcomes differ")
        log.write(f"PASS: {len(signatures)} simulator traces and summaries match exactly.\n")
    print(signatures[0][1])
    print(f"Simulator replay verified: {args.output}")


if __name__ == "__main__":
    main()
