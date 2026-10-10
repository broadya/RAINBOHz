#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

# reset_db.sh
# Destroy the engine LadybugDB database and reinitialise it from schema.
#
# WARNING: all data is permanently deleted. Use init_db.sh to apply schema
# to an existing database without destroying data.
#
# Usage:
#   ./reset_db.sh [-y] [db_path]
#
#   -y   Skip the confirmation prompt.
#
# Resolution order for db_path:
#   1. First non-flag positional argument
#   2. $ENGINE_DB_PATH environment variable
#   3. ./db/engine.lbug (default)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCHEMA_DIR="${SCHEMA_DIR:-${SCRIPT_DIR}/schema}"
DEFAULT_DB_PATH="${SCRIPT_DIR}/db/engine.lbug"

SKIP_CONFIRM=0
DB_PATH_ARG=""

for arg in "$@"; do
    case "$arg" in
        -y|--yes) SKIP_CONFIRM=1 ;;
        *)        DB_PATH_ARG="$arg" ;;
    esac
done

DB_PATH="${DB_PATH_ARG:-${ENGINE_DB_PATH:-${DEFAULT_DB_PATH}}}"

# Not an array: macOS ships bash 3.2, where expanding an empty array under
# `set -u` is an unbound-variable error.
if [[ "$SKIP_CONFIRM" -eq 1 ]]; then
    exec "${PYTHON:-python3}" "${SCRIPT_DIR}/../scripts/load_schema.py" \
        --schema-dir "$SCHEMA_DIR" --db "$DB_PATH" --reset --yes
else
    exec "${PYTHON:-python3}" "${SCRIPT_DIR}/../scripts/load_schema.py" \
        --schema-dir "$SCHEMA_DIR" --db "$DB_PATH" --reset
fi
