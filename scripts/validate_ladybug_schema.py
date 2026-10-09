#!/usr/bin/env python3
"""
100% LLM-generated code

validate_ladybug_schema.py

Validates one or more LadybugDB DDL schema files by actually executing them
against a real, throwaway LadybugDB instance. This is a *behavioural* check,
not a parser reimplementation: whatever the installed `lbug` accepts is the
definition of valid.

The statement splitting and execution machinery is shared with the database
loader -- see scripts/ladybug_ddl.py, which also documents why statements must
be submitted to `lbug` one at a time.

Usage:
    python validate_ladybug_schema.py schema.cypher [more.cypher ...]

    # Show every statement, not just the failures:
    python validate_ladybug_schema.py schema.cypher --verbose

Exit status is 0 only if every statement in every file executed cleanly.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile

# The shared DDL machinery lives alongside, in scripts/.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from ladybug_ddl import (  # noqa: E402
        Statement,
        find_lbug,
        run_statement,
        show_tables,
        split_statements,
    )
except ImportError:  # pragma: no cover
    sys.exit(
        "Error: cannot import ladybug_ddl.\n"
        "       Expected it at "
        f"{os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ladybug_ddl.py')}"
    )


def validate(path: str, lbug: str, verbose: bool) -> bool:
    with open(path, encoding="utf-8") as handle:
        source = handle.read()

    statements = split_statements(source)
    workdir = tempfile.mkdtemp(prefix="ladybug-validate-")
    db_path = os.path.join(workdir, "validate.lbug")

    print(f"\n{'=' * 70}")
    print(f"Validating {path}")
    print(f"  {len(statements)} statement(s), lbug: {lbug}")
    print("=" * 70)

    failures: list[tuple[Statement, str]] = []
    try:
        for statement in statements:
            result = run_statement(lbug, db_path, statement.text)
            if result.ok:
                if verbose:
                    print(f"  ok    {statement.lines}: {statement.label}")
            else:
                # Validation runs against an empty database, so "already
                # exists" is a genuine duplicate declaration, not a re-apply.
                failures.append((statement, result.diagnostic))
                print(f"  FAIL  {statement.lines}: {statement.label}")
                for detail in result.diagnostic.splitlines():
                    print(f"          {detail}")

        created = show_tables(lbug, db_path)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    nodes = [name for name, kind in created if kind == "NODE"]
    rels = [name for name, kind in created if kind == "REL"]

    print(f"\n  Statements: {len(statements) - len(failures)} ok, {len(failures)} failed")
    print(f"  Tables created: {len(nodes)} NODE, {len(rels)} REL")

    if failures:
        print(f"\n  {os.path.basename(path)}: INVALID")
        return False

    print(f"\n  {os.path.basename(path)}: valid")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate LadybugDB DDL by executing it against a "
        "throwaway database.",
    )
    parser.add_argument("schema", nargs="+", help="one or more .cypher DDL files")
    parser.add_argument(
        "--lbug",
        default=os.environ.get("LBUG", "lbug"),
        help="path to the lbug CLI (default: $LBUG, else 'lbug' on PATH)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="report every statement, not only the failures",
    )
    args = parser.parse_args()

    lbug = find_lbug(args.lbug)
    if lbug is None:
        print(f"Error: lbug CLI not found: {args.lbug}", file=sys.stderr)
        print("       brew install ladybug", file=sys.stderr)
        return 2

    results = [validate(path, lbug, args.verbose) for path in args.schema]

    print()
    if not all(results):
        failed = sum(1 for ok in results if not ok)
        print(f"FAILED: {failed} of {len(results)} schema file(s) are invalid.")
        return 1

    print(f"All {len(results)} schema file(s) valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
