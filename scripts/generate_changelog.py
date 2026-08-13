#!/usr/bin/env python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

"""Generate a reviewable Keep a Changelog draft from commit subjects."""

import argparse
from collections import defaultdict
import subprocess


SECTIONS = {
    "feat": "Added",
    "fix": "Fixed",
    "perf": "Changed",
    "refactor": "Changed",
    "docs": "Documentation",
    "test": "Testing",
    "build": "Build",
    "ci": "Build",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--since", help="oldest excluded Git tag or revision")
    args = parser.parse_args()
    revision = f"{args.since}..HEAD" if args.since else "HEAD"
    process = subprocess.run(
        ["git", "log", revision, "--pretty=format:%s"], check=True,
        text=True, stdout=subprocess.PIPE)
    grouped: dict[str, list[str]] = defaultdict(list)
    for subject in process.stdout.splitlines():
        prefix, separator, remainder = subject.partition(":")
        kind = prefix.split("(", 1)[0].rstrip("!") if separator else ""
        grouped[SECTIONS.get(kind, "Changed")].append(
            remainder.strip() if separator else subject)
    print("## [Unreleased]\n")
    for section in ("Added", "Changed", "Fixed", "Documentation", "Testing", "Build"):
        if not grouped[section]:
            continue
        print(f"### {section}\n")
        for subject in grouped[section]:
            print(f"- {subject}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
