#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""
100% LLM-generated code

load_schema.py

Applies LadybugDB schema DDL to a database. Used by the per-component
init_db.sh / reset_db.sh wrappers, and by the Makefile's init-* / reset-*
targets.

This replaces piping schema files straight into `lbug`, which cannot be used
as a batch loader: it reports errors on stdout, exits 0 regardless, and
silently skips statements when fed a whole file. See ladybug_ddl.py.

Idempotency
-----------
The schema files do not use `CREATE ... IF NOT EXISTS`, so re-applying a schema
to a populated database produces "already exists in catalog" for every table.
Those are reported as "already present" and do not fail the run; every other
error does. A run is therefore safe to repeat, but note that it applies only
*new* declarations -- it does not alter tables that already exist. Per
AGENTS.md, schema migration during early development is drop-and-recreate:
use --reset for that.

Usage:
    load_schema.py --schema-dir engine-db/schema --db engine-db/db/engine.lbug
    load_schema.py --schema-dir canonical-db/schema --db /tmp/c.lbug --reset --yes
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ladybug_ddl import (  # noqa: E402
    Statement,
    find_lbug,
    run_statement,
    show_tables,
    split_statements,
)


def collect_schema_files(schema_dir: str | None, explicit: list[str]) -> list[str]:
    """Schema files, directory contents in lexicographic order."""
    files = list(explicit)
    if schema_dir:
        if not os.path.isdir(schema_dir):
            raise SystemExit(f"Error: schema directory not found: {schema_dir}")
        found = sorted(
            os.path.join(schema_dir, name)
            for name in os.listdir(schema_dir)
            if name.endswith(".cypher")
        )
        if not found:
            raise SystemExit(f"Error: no .cypher files in {schema_dir}")
        files.extend(found)
    if not files:
        raise SystemExit("Error: no schema files given (use --schema-dir or --schema)")
    return files


def confirm(db_path: str, assume_yes: bool) -> bool:
    if assume_yes:
        return True
    if not sys.stdin.isatty():
        print(
            "Error: refusing to delete a database without confirmation.\n"
            "       Re-run with --yes to proceed non-interactively.",
            file=sys.stderr,
        )
        return False
    print("WARNING: this will permanently delete the database at:")
    print(f"         {db_path}")
    return input("Type 'yes' to continue: ").strip() == "yes"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply LadybugDB schema DDL to a database.",
    )
    parser.add_argument("--db", required=True, help="path to the database")
    parser.add_argument("--schema-dir", help="directory of .cypher files")
    parser.add_argument(
        "--schema", action="append", default=[], help="an individual .cypher file"
    )
    parser.add_argument(
        "--reset",
        action="store_true",
        help="delete the database first (destroys all data)",
    )
    parser.add_argument(
        "--yes", "-y", action="store_true", help="skip the --reset confirmation prompt"
    )
    parser.add_argument(
        "--lbug",
        default=os.environ.get("LBUG", "lbug"),
        help="path to the lbug CLI (default: $LBUG, else 'lbug' on PATH)",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="report every statement"
    )
    args = parser.parse_args()

    lbug = find_lbug(args.lbug)
    if lbug is None:
        print(f"Error: lbug CLI not found: {args.lbug}", file=sys.stderr)
        print("       brew install ladybug", file=sys.stderr)
        return 2

    files = collect_schema_files(args.schema_dir, args.schema)
    db_path = os.path.abspath(args.db)

    if args.reset:
        if os.path.exists(db_path):
            if not confirm(db_path, args.yes):
                print("Aborted.", file=sys.stderr)
                return 1
            print(f"Deleting: {db_path}")
            if os.path.isdir(db_path):
                shutil.rmtree(db_path)
            else:
                os.remove(db_path)
        else:
            print(f"Nothing to delete (does not exist): {db_path}")

    os.makedirs(os.path.dirname(db_path) or ".", exist_ok=True)

    print(f"Database: {db_path}")
    print(f"lbug:     {lbug}")
    print()

    created = 0
    skipped = 0
    failures: list[tuple[str, Statement, str]] = []

    for path in files:
        with open(path, encoding="utf-8") as handle:
            statements = split_statements(handle.read())
        print(f"  applying {os.path.basename(path)} ({len(statements)} statements)...")

        for statement in statements:
            result = run_statement(lbug, db_path, statement.text)
            if result.ok:
                created += 1
                if args.verbose:
                    print(f"    ok       {statement.lines}: {statement.label}")
            elif result.already_exists:
                # Re-applying to a populated database; not an error.
                skipped += 1
                if args.verbose:
                    print(f"    present  {statement.lines}: {statement.label}")
            else:
                failures.append((path, statement, result.diagnostic))
                print(f"    FAILED   {statement.lines}: {statement.label}")
                for detail in result.diagnostic.splitlines():
                    print(f"               {detail}")

    tables = show_tables(lbug, db_path)
    nodes = sum(1 for _, kind in tables if kind == "NODE")
    rels = sum(1 for _, kind in tables if kind == "REL")

    print()
    print(f"  Applied: {created}, already present: {skipped}, failed: {len(failures)}")
    print(f"  Database now holds {nodes} NODE and {rels} REL tables.")

    if failures:
        print(f"\nFAILED: {len(failures)} statement(s) did not apply.", file=sys.stderr)
        print("The database is in a partially-loaded state.", file=sys.stderr)
        return 1

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
