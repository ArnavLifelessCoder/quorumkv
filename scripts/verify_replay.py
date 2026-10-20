"""Run the same fuzz seed twice and compare its deterministic corpus fingerprint."""

import argparse
import pathlib
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=pathlib.Path)
    arguments = parser.parse_args()
    executable = arguments.executable.resolve(strict=True)
    root = pathlib.Path(__file__).resolve().parent.parent
    output = root / "results" / "m1" / "replay.txt"
    output.parent.mkdir(parents=True, exist_ok=True)
    fingerprints = []
    with output.open("w", encoding="utf-8", newline="\n") as log:
        for attempt in range(2):
            result = subprocess.run(
                [str(executable), "--seed=42", "--gtest_filter=CodecFuzz.*"],
                cwd=root,
                capture_output=True,
                text=True,
                check=True,
            )
            log.write(f"Replay attempt {attempt + 1}\n{result.stdout}{result.stderr}\n")
            lines = [line for line in result.stdout.splitlines() if line.startswith("[fuzz]")]
            if len(lines) != 1:
                raise RuntimeError("expected exactly one fuzz summary")
            fingerprints.append(lines[0])
        if fingerprints[0] != fingerprints[1]:
            raise RuntimeError("the same seed produced different corpora")
        invalid = subprocess.run(
            [str(executable), "--seed=-1"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        log.write(
            f"Invalid CLI seed: exit={invalid.returncode}\n{invalid.stdout}{invalid.stderr}\n"
        )
        if invalid.returncode != 2:
            raise RuntimeError("invalid seed was not rejected with exit code 2")
        log.write("PASS: both replay fingerprints match; invalid CLI seed is rejected.\n")
    print(fingerprints[0])
    print(f"Replay verification passed. Full output: {output}")


if __name__ == "__main__":
    main()
