#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Compiles hzl-compiler's example scores to tables, for the engine's tests.

The tests that compare the two engine-db backends (ADR 0028) need scores as
hzlc writes them, and only Parquet tables load into both: Ladybug copies the
files, the native backend reads them itself. CTest runs this once, as the
setup of the `compiled_scores` fixture, into the build tree; each example
lands in `<out-dir>/<example>/manifest.json`.

Usage:
    compile_scores.py --examples <hzl-compiler/examples> --out-dir <dir>
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

EXAMPLES = (
    ("tempo-map-score", "tempo_map_score.yaml", "0"),
    ("polytempo-shift-score", "polytempo_shift_score.yaml", "0"),
    ("polytempo-stretch-score", "polytempo_stretch_score.yaml", "0"),
    ("spatial-score", "spatial_score.yaml", "1"),
    ("demo", "demo.yaml", "0"),
)
"""Each example: its directory, its score file and the channel mode it is
compiled with, as `make test-score-forms` compiles it."""


def main() -> int:
    """Compiles every example.

    Returns:
        0 when every example compiled; 1 otherwise, with hzlc's output.
    """
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--examples", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()

    out: Path = args.out_dir
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for name, score, mode in EXAMPLES:
        command = [
            sys.executable,
            "-m",
            "hzl_compiler",
            "compile",
            str(args.examples / name / "scores" / score),
            "-q",
            "--channel-mode",
            mode,
            "-o",
            str(out / name),
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            print(f"failed: {' '.join(command)}\n{result.stdout}{result.stderr}", file=sys.stderr)
            return 1
    print(f"compiled {len(EXAMPLES)} example score(s) into {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
