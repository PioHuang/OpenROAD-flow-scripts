#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "usage: $0 <2_floorplan.odb> <2_floorplan.sdc> <out_dir> [openroad_bin]" >&2
  exit 2
fi

ODB_FILE="$(realpath "$1")"
SDC_FILE="$(realpath "$2")"
OUT_DIR="$(realpath "$3")"
OPENROAD_BIN="${4:-openroad}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPTS_DIR="$ROOT_DIR/flow/scripts"
TCL_SCRIPT="$ROOT_DIR/research/scripts/extract_floorplan_db.tcl"

mkdir -p "$OUT_DIR"

export ODB_FILE
export SDC_FILE
export OUT_DIR
export SCRIPTS_DIR

# OpenROAD in conda may pick an older libstdc++; preload system one if available.
SYS_LIBSTDCXX="/usr/lib/x86_64-linux-gnu/libstdc++.so.6"
if [[ -f "$SYS_LIBSTDCXX" ]]; then
  export LD_PRELOAD="${SYS_LIBSTDCXX}${LD_PRELOAD:+:$LD_PRELOAD}"
fi

"$OPENROAD_BIN" -no_init -exit "$TCL_SCRIPT"

echo "Extraction complete: $OUT_DIR"
