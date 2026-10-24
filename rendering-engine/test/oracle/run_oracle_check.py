#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""CTest driver: render a score with the engine, then check every frame with the oracle.

Runs rendering-engine in batch mode on one score and one request, with
--per-set-wav, then runs render_oracle.py on every frame the request names,
both the whole frame and each partial set's file where one was written.

    run_oracle_check.py --engine <binary> --schema <cypher> --state <cypher>
        --request <yaml> --score <json> --out-dir <dir> [--render-kernel simd|scalar]

Exit status 0 when the engine and every oracle run succeed, 1 on any failure,
including NumPy not being installed: a missing package fails the check rather
than skipping it, so a gap cannot pass as green.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

def requested_frames(request_path: str) -> list[int]:
    """Read the frame numbers a request names.

    The request format is small enough to read without a YAML library: every
    entry is a line `- frame: <n>` (docs/design.md section 3.2).

    Args:
        request_path: The YAML request file.

    Returns:
        The frame numbers, in file order.
    """
    frames = []
    with open(request_path, encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^\s*-\s*frame:\s*(\d+)\s*(#.*)?$", line)
            if match:
                frames.append(int(match.group(1)))
    return frames


def main() -> int:
    """Run the engine, then the oracle per frame and per set; report."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--request", required=True)
    parser.add_argument("--score", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument(
        "--render-kernel", choices=("simd", "scalar"), default="simd",
        help="the engine's kernel: simd (its default) or the scalar reference (ADR 0027)",
    )
    args = parser.parse_args()

    try:
        import numpy  # noqa: F401
    except ImportError:
        print("run_oracle_check: FAILED: NumPy is not installed; run `make venv` at the repository root", file=sys.stderr)
        return 1

    shutil.rmtree(args.out_dir, ignore_errors=True)
    os.makedirs(args.out_dir)
    engine = subprocess.run(
        [
            args.engine, "--mode", "batch", "--schema", args.schema, "--state", args.state,
            "--request", args.request, "--out-dir", args.out_dir, "--per-set-wav",
            "--render-kernel", args.render_kernel,
        ],
        capture_output=True, text=True, check=False,
    )
    if engine.returncode != 0:
        print(engine.stdout)
        print(engine.stderr, file=sys.stderr)
        print(f"run_oracle_check: the engine exited {engine.returncode}", file=sys.stderr)
        return 1

    with open(args.score, encoding="utf-8") as handle:
        set_ids = [int(s["id"]) for s in json.load(handle)["partial_sets"]]
    oracle = os.path.join(os.path.dirname(os.path.abspath(__file__)), "render_oracle.py")

    failures = 0
    for frame in requested_frames(args.request):
        checks = [(os.path.join(args.out_dir, f"frame-{frame:06d}.wav"), [])]
        for set_id in set_ids:
            per_set = os.path.join(args.out_dir, f"frame-{frame:06d}.set-{set_id:06d}.wav")
            if os.path.exists(per_set):  # written only when the set contributed
                checks.append((per_set, ["--set", str(set_id)]))
        for wav, extra in checks:
            result = subprocess.run(
                [sys.executable, oracle, "--score", args.score, "--frame", str(frame), "--wav", wav, *extra],
                capture_output=True, text=True, check=False,
            )
            print(result.stdout, end="")
            if result.returncode != 0:
                print(result.stderr, end="", file=sys.stderr)
                failures += 1
    if failures:
        print(f"run_oracle_check: {failures} oracle check(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
