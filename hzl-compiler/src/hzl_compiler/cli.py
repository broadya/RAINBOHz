# SPDX-License-Identifier: Apache-2.0

"""The `hzlc` command line: compile a score to Cypher."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import __version__
from .canonical.cypher import emit_cypher
from .canonical.model import TABLE_ORDER
from .errors import HzlError
from .lower.score import CHANNEL_MODES, EngineOptions, lower_score
from .source import SourceTree


def build_parser() -> argparse.ArgumentParser:
    """Builds the argument parser for `hzlc`."""
    parser = argparse.ArgumentParser(
        prog="hzlc", description="Compile an HzL score to Cypher for engine-db."
    )
    parser.add_argument("--version", action="version", version=f"hzlc {__version__}")
    commands = parser.add_subparsers(dest="command", required=True)

    compile_ = commands.add_parser(
        "compile", help="compile a score file and everything it references"
    )
    compile_.add_argument(
        "score",
        type=Path,
        help="the score's .yaml file; the source root is worked out from its namespace",
    )
    compile_.add_argument(
        "-o", "--output", type=Path, required=True, help="the .cypher file to write"
    )
    compile_.add_argument(
        "--sample-rate",
        type=int,
        default=48000,
        help="EngineConfig.sampleRate in Hz (default 48000)",
    )
    compile_.add_argument(
        "--channel-mode",
        type=int,
        default=0,
        choices=sorted(CHANNEL_MODES),
        help="EngineConfig.channelMode: " + ", ".join(f"{k} {v}" for k, v in CHANNEL_MODES.items()),
    )
    compile_.add_argument(
        "--dump-canonical",
        type=Path,
        metavar="FILE.json",
        help="also write the canonical model's tables as JSON",
    )
    compile_.add_argument("-q", "--quiet", action="store_true", help="print nothing on success")
    return parser


def compile_command(args: argparse.Namespace) -> int:
    """Runs `hzlc compile`.

    Writes the Cypher script to `args.output`, creating its directory, and
    the JSON dump if asked. Prints a one-line summary to stdout unless quiet.

    Returns:
        0 on success, 1 on an HzL fault (reported on stderr).
    """
    try:
        options = EngineOptions(sample_rate=args.sample_rate, channel_mode=args.channel_mode)
    except ValueError as error:
        print(f"hzlc: error: {error}", file=sys.stderr)
        return 2
    try:
        tree, ref = SourceTree.from_file(args.score)
        model = lower_score(tree, ref, options)
    except HzlError as error:
        print(f"hzlc: error: {error}", file=sys.stderr)
        return 1
    script = emit_cypher(
        model,
        [
            f"Score {ref}, from source tree {tree.root.name}",
            (
                f"hzlc {__version__}; sample rate {options.sample_rate} Hz, "
                f"channel mode {options.channel_mode} ({CHANNEL_MODES[options.channel_mode]})"
            ),
        ],
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(script, encoding="utf-8")
    if args.dump_canonical is not None:
        args.dump_canonical.parent.mkdir(parents=True, exist_ok=True)
        args.dump_canonical.write_text(
            json.dumps(model.to_dict(), indent=2) + "\n", encoding="utf-8"
        )
    if not args.quiet:
        counts = ", ".join(
            f"{model.count(t)} {t}" for t in TABLE_ORDER if model.count(t) and t != "EngineConfig"
        )
        print(f"hzlc: compiled {ref}: {counts}; {len(model.edges)} relationships -> {args.output}")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point: parses `argv` (or `sys.argv[1:]`) and runs the command.

    Returns:
        The process exit code: 0 success, 1 HzL fault, 2 usage.
    """
    args = build_parser().parse_args(argv)
    if args.command == "compile":
        return compile_command(args)
    return 2
