#!/usr/bin/env bash
# init_db.sh
# Initialise the canonical-form LadybugDB database from schema files.
#
# Safe to run repeatedly: declarations that already exist are reported as
# "already present" rather than failing. Note that this applies only *new*
# declarations -- it does not alter tables that already exist. Per AGENTS.md,
# schema migration during early development is drop-and-recreate: use
# reset_db.sh for that.
#
# Usage:
#   ./init_db.sh [db_path]
#
# Resolution order for db_path:
#   1. First positional argument
#   2. $CANONICAL_DB_PATH environment variable
#   3. ./db/canonical.lbug (default)
#
# Schema files are read from ./schema/ in lexicographic order.
#
# The real work is done by ../scripts/load_schema.py. Schema DDL must not be
# piped straight into `lbug`: it reports errors on stdout, exits 0 regardless,
# and silently skips statements when fed a whole file. See scripts/ladybug_ddl.py.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCHEMA_DIR="${SCHEMA_DIR:-${SCRIPT_DIR}/schema}"
DEFAULT_DB_PATH="${SCRIPT_DIR}/db/canonical.lbug"

DB_PATH="${1:-${CANONICAL_DB_PATH:-${DEFAULT_DB_PATH}}}"

exec "${PYTHON:-python3}" "${SCRIPT_DIR}/../scripts/load_schema.py" \
    --schema-dir "$SCHEMA_DIR" \
    --db "$DB_PATH"
