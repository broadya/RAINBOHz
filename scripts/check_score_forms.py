#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Checks that a score's two compiled forms render the same audio.

hzlc writes a score as Parquet tables and a manifest, or with
`--debug-cypher` as one Cypher script. The engine loads either, and the two
must give the same engine-db: the same ids in particular, since
relationships name their ends by id (ADR 0025). This compiles one score both
ways, renders the same request from each, and fails unless every frame file
is byte-identical.

Usage:
    check_score_forms.py --engine <binary> --schema <cypher> --score <score.yaml> \\
        --request <request.yaml> --out-dir <dir> [--channel-mode N]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path


def run(command: list[str]) -> float:
    """Runs `command`, failing on a non-zero exit; returns its wall time in seconds."""
    start = time.perf_counter()
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"failed: {' '.join(command)}\n{result.stdout}{result.stderr}")
    return time.perf_counter() - start


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--score", required=True)
    parser.add_argument("--request", required=True)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--channel-mode", default="0")
    args = parser.parse_args()

    out: Path = args.out_dir
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    hzlc = [
        sys.executable,
        "-m",
        "hzl_compiler",
        "compile",
        args.score,
        "-q",
        "--channel-mode",
        args.channel_mode,
    ]
    run([*hzlc, "-o", str(out / "tables")])
    run([*hzlc, "-o", str(out / "score.cypher"), "--debug-cypher"])

    seconds = {}
    for form, state in (
        ("tables", out / "tables" / "manifest.json"),
        ("cypher", out / "score.cypher"),
    ):
        seconds[form] = run(
            [
                args.engine,
                "--mode",
                "batch",
                "--schema",
                args.schema,
                "--state",
                str(state),
                "--request",
                args.request,
                "--out-dir",
                str(out / f"frames-{form}"),
            ]
        )

    tables = sorted(p.name for p in (out / "frames-tables").iterdir())
    cypher = sorted(p.name for p in (out / "frames-cypher").iterdir())
    if not tables or tables != cypher:
        sys.exit(f"{args.score}: the forms wrote different files: {tables} vs {cypher}")
    differ = [
        name
        for name in tables
        if (out / "frames-tables" / name).read_bytes()
        != (out / "frames-cypher" / name).read_bytes()
    ]
    if differ:
        sys.exit(f"{args.score}: frames differ between the forms: {', '.join(differ)}")
    print(
        f"{Path(args.score).stem}: {len(tables)} frame file(s) byte-identical; "
        f"run {seconds['tables']:.2f} s from tables, {seconds['cypher']:.2f} s from Cypher"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
