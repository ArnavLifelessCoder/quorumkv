"""Check formatting of tracked C++ source without scanning build dependencies."""

import pathlib
import subprocess


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")
    sources = [name for name in tracked if pathlib.Path(name).suffix in {".cpp", ".hpp"}]
    if sources:
        subprocess.run(
            ["clang-format", "--dry-run", "--Werror", *sources],
            cwd=root,
            check=True,
        )


if __name__ == "__main__":
    main()
