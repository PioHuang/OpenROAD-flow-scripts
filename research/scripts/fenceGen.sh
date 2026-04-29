#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# fenceGen: inject DEF REGIONS (+ TYPE FENCE) from RTLMP root.fp.txt and add
# + REGION to each matching GROUP. Paths in mempool.json are relative to ORFS root.
#
# Usage:
#   ./research/scripts/fenceGen.sh \
#     research/mempool.json \
#     flow/results/nangate45/mempool_group/base/2_floorplan.def \
#     flow/results/nangate45/mempool_group/base/2_floorplan_fenced.def
#
# Optional extra args are passed to the Python script (e.g. --force).

set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "usage: $0 <mempool.json> <input.def> <output.def> [-- extra python args...]" >&2
  exit 2
fi

JSON="$(realpath "$1")"
DEF_IN="$(realpath "$2")"
DEF_OUT="$(realpath "$3")"
shift 3

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="$ROOT_DIR/research/scripts/fenceGen.py"

exec python3 "$PY" --root "$ROOT_DIR" --json "$JSON" --def-in "$DEF_IN" --def-out "$DEF_OUT" "$@"
