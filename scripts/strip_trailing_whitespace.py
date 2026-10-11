#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""
100% LLM-generated code

strip_trailing_whitespace.py

Finds, and optionally removes, spaces and tabs at the end of lines in the
repository's tracked text files. Nothing else in a file changes: line endings
(LF or CRLF) and the presence or absence of a final newline are kept.

Which files:

  * Every file `git ls-files` lists, run from the repository root.
  * Binary files are skipped: any file containing a NUL byte, or that is not
    UTF-8.
  * The frozen proof-of-concept directories are skipped
    (proof-of-concept/{audio-rendering,hzl-language,maths}/). They are kept
    as they were written.
  * README.md is skipped: it is generated from docs/readme/ by `make readme`,
    so fix the source and regenerate.

Markdown note: two trailing spaces are a hard line break in Markdown. This
script removes them like any other trailing whitespace; write a hard break
as a trailing backslash instead.

Usage:
    # Report the offending lines, write nothing, exit 1 if there are any:
    python strip_trailing_whitespace.py

    # Rewrite the offending files in place:
    python strip_trailing_whitespace.py --apply

Running it twice changes nothing the second time.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

FROZEN = re.compile(r"^proof-of-concept/(audio-rendering|hzl-language|maths)/")
GENERATED = {"README.md"}

# Trailing spaces and tabs before an optional CR, at the end of each line.
TRAILING = re.compile(r"[ \t]+(?=\r?$)", re.MULTILINE)


def repository_root() -> Path:
    """Returns the top of the git work tree the script is run in."""
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
        check=True,
    )
    return Path(out.stdout.strip())


def candidate_files(root: Path) -> list[str]:
    """Returns the tracked paths, relative to `root`, that the script covers."""
    out = subprocess.run(
        ["git", "ls-files", "-z"], cwd=root, capture_output=True, text=True, check=True
    )
    return [
        f
        for f in out.stdout.split("\0")
        if f and not FROZEN.match(f) and f not in GENERATED and (root / f).is_file()
    ]


def read_text(path: Path) -> str | None:
    """Returns the file's text, or None if it is binary."""
    data = path.read_bytes()
    if b"\0" in data:
        return None
    try:
        # Decoding bytes keeps any CR, so CRLF files are written back as CRLF.
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def main(argv: list[str] | None = None) -> int:
    """Checks or fixes every covered file.

    Returns:
        0 if nothing had trailing whitespace (or --apply removed it all),
        1 if --check found some.
    """
    parser = argparse.ArgumentParser(
        description="Find or remove trailing whitespace in tracked text files."
    )
    parser.add_argument(
        "--apply", action="store_true", help="rewrite the files instead of reporting"
    )
    args = parser.parse_args(argv)

    root = repository_root()
    offending = 0
    for name in candidate_files(root):
        path = root / name
        text = read_text(path)
        if text is None:
            continue
        hits = [
            number
            for number, line in enumerate(text.split("\n"), start=1)
            if TRAILING.search(line)
        ]
        if not hits:
            continue
        offending += 1
        if args.apply:
            path.write_bytes(TRAILING.sub("", text).encode("utf-8"))
            print(f"{name}: stripped {len(hits)} line(s)")
        else:
            for number in hits:
                print(f"{name}:{number}: trailing whitespace")

    if offending and not args.apply:
        print(
            f"{offending} file(s) with trailing whitespace; "
            "fix with scripts/strip_trailing_whitespace.py --apply",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
