#!/usr/bin/env python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

"""Create a byte-reproducible source archive and SHA-256 checksum."""

import argparse
import gzip
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tarfile


EXCLUDED_PARTS = {
    ".git", "CMakeFiles", "outputs", "release", "release-a", "release-b",
    "work", "__pycache__"
}
EXCLUDED_PREFIXES = ("build",)


def source_files(root: Path) -> list[Path]:
    tracked = subprocess.run(
        ["git", "ls-files", "-z"], cwd=root, check=False,
        stdout=subprocess.PIPE).stdout.split(b"\0")
    paths = [root / os.fsdecode(item) for item in tracked if item]
    if paths:
        return sorted(path for path in paths if path.is_file())
    return sorted(
        path for path in root.rglob("*")
        if path.is_file()
        and not any(part in EXCLUDED_PARTS or part.startswith(EXCLUDED_PREFIXES)
                    for part in path.relative_to(root).parts)
        and path.name != ".DS_Store")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?",
                        args.version):
        parser.error("version must be a semantic version without a leading v")

    root = Path(__file__).resolve().parents[1]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    archive = args.output_dir / f"yang-cpp-{args.version}.tar.gz"
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    prefix = f"yang-cpp-{args.version}"
    with archive.open("wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=epoch) as zipped:
            with tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for path in source_files(root):
                    relative = path.relative_to(root)
                    info = tar.gettarinfo(str(path), f"{prefix}/{relative.as_posix()}")
                    info.mtime = epoch
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "root"
                    info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
                    with path.open("rb") as source:
                        tar.addfile(info, source)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (args.output_dir / "SHA256SUMS").write_text(
        f"{digest}  {archive.name}\n", encoding="utf-8", newline="\n")
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
