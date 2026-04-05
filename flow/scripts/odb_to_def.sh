#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <input.odb> <output.def>" >&2
  exit 1
fi

ODB_PATH="$1"
DEF_PATH="$2"

if [[ ! -f "$ODB_PATH" ]]; then
  echo "Error: input ODB does not exist: $ODB_PATH" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ODB_FILE="$ODB_PATH" \
DEF_FILE="$DEF_PATH" \
openroad -exit "$SCRIPT_DIR/write_def.tcl"

echo "Wrote DEF: $DEF_PATH"
