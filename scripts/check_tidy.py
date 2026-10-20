"""Check every tracked translation unit for this platform and retain complete output."""

import argparse
import concurrent.futures
import json
import os
import pathlib
import shutil
import subprocess


def find_tidy() -> str:
    installed = shutil.which("clang-tidy")
    if installed:
        return installed
    if vc_root := os.environ.get("VCINSTALLDIR"):
        candidate = pathlib.Path(vc_root) / "Tools" / "Llvm" / "x64" / "bin" / "clang-tidy.exe"
        if candidate.is_file():
            return str(candidate)
    raise FileNotFoundError("clang-tidy is unavailable; use a developer shell or --clang-tidy")


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, default=root / "build" / "debug")
    parser.add_argument(
        "--output", type=pathlib.Path, default=root / "build" / "review" / "clang-tidy.txt"
    )
    parser.add_argument("--clang-tidy")
    parser.add_argument("--jobs", type=int, default=2)
    arguments = parser.parse_args()
    executable = arguments.clang_tidy or find_tidy()
    sources = subprocess.check_output(
        ["git", "ls-files", "-z", "*.cpp"], cwd=root, text=True
    ).split("\0")
    sources = [str((root / source).resolve()) for source in sources if source]
    database = json.loads(
        (arguments.build_dir / "compile_commands.json").read_text(encoding="utf-8")
    )
    compiled = {pathlib.Path(entry["file"]).resolve() for entry in database}
    other_platform = root / "src" / "platform" / ("posix" if os.name == "nt" else "windows")
    excluded = [source for source in sources if pathlib.Path(source).is_relative_to(other_platform)]
    sources = [source for source in sources if source not in excluded]
    if not sources or any(pathlib.Path(source) not in compiled for source in sources):
        raise ValueError("configure the debug build with all tracked translation units first")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([executable, "--version"], text=True)

    def check(source: str):
        return subprocess.run(
            [executable, "-p", str(arguments.build_dir.resolve()), source],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        results = list(pool.map(check, sources))
    exit_code = int(any(result.returncode != 0 for result in results))
    text = (
        version
        + "\nTranslation units:\n"
        + "\n".join(sources)
        + "\n\nOther-platform translation units (checked on that platform):\n"
        + "\n".join(excluded)
        + "\n\n"
        + "\n".join(
            f"Translation unit: {source}\n{result.stdout}{result.stderr}"
            f"\nTranslation unit exit code: {result.returncode}\n"
            for source, result in zip(sources, results, strict=True)
        )
        + f"\nclang-tidy exit code: {exit_code}\n"
    )
    arguments.output.write_text(text, encoding="utf-8")
    print(text)
    print(f"Full clang-tidy output: {arguments.output}")
    raise SystemExit(exit_code)


if __name__ == "__main__":
    main()
