#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Checks that a score's two compiled forms render the same audio on both backends.

hzlc writes a score as Parquet tables and a manifest, or with
`--debug-cypher` as one Cypher script. The engine loads either, and the two
must give the same engine-db: the same ids in particular, since
relationships name their ends by id (ADR 0025). The tables load into either
engine-db backend, the Cypher into Ladybug only, and the backends must agree
(ADR 0028). The engine must also write the same frames whatever its
--render-threads (ADR 0026). This compiles one score both ways and renders
the same request four times: from the tables natively with one thread and
with eight, from the tables into Ladybug with eight, and from the Cypher with
eight. It fails unless every frame file is byte-identical across the four.

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

    # Each run: its name, the score form, the thread count, and the backend
    # (None: the score decides, so the tables load natively and the Cypher
    # into Ladybug; ADR 0028).
    runs: tuple[tuple[str, Path, str, str | None], ...] = (
        ("native-t1", out / "tables" / "manifest.json", "1", None),
        ("native-t8", out / "tables" / "manifest.json", "8", None),
        ("ladybug-tables-t8", out / "tables" / "manifest.json", "8", "ladybug"),
        ("ladybug-cypher-t8", out / "score.cypher", "8", None),
    )
    seconds = {}
    for name, state, threads, backend in runs:
        seconds[name] = run(
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
                "--render-threads",
                threads,
                "--out-dir",
                str(out / f"frames-{name}"),
                *(["--backend", backend] if backend else []),
            ]
        )

    reference = runs[0][0]
    files = sorted(p.name for p in (out / f"frames-{reference}").iterdir())
    if not files:
        sys.exit(f"{args.score}: no frames written")
    for name, _, _, _ in runs[1:]:
        other = sorted(p.name for p in (out / f"frames-{name}").iterdir())
        if other != files:
            sys.exit(f"{args.score}: {name} wrote different files: {other} vs {files}")
        differ = [
            file
            for file in files
            if (out / f"frames-{name}" / file).read_bytes()
            != (out / f"frames-{reference}" / file).read_bytes()
        ]
        if differ:
            sys.exit(
                f"{args.score}: frames differ between {reference} and {name}: {', '.join(differ)}"
            )
    print(
        f"{Path(args.score).stem}: {len(files)} frame file(s) byte-identical; run "
        + ", ".join(f"{seconds[name]:.2f} s {name}" for name, _, _, _ in runs)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
