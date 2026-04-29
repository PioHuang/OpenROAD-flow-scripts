#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <odb_file> <sdc_file> <out_dir>"
  exit 1
fi

ODB_FILE="$1"
SDC_FILE="$2"
OUT_DIR="$3"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TCL_SCRIPT="${SCRIPT_DIR}/extract_floorplan_db.tcl"
SCRIPTS_DIR="${SCRIPT_DIR}/../flow/scripts"

export ODB_FILE SDC_FILE OUT_DIR SCRIPTS_DIR

SYS_LIBSTDCXX="/usr/lib/x86_64-linux-gnu/libstdc++.so.6"
if [[ -f "$SYS_LIBSTDCXX" ]]; then
  export LD_PRELOAD="${SYS_LIBSTDCXX}${LD_PRELOAD:+:$LD_PRELOAD}"
fi

OPENROAD_CMD="${OPENROAD_BIN:-openroad}"
if [[ -d "$OPENROAD_CMD" ]]; then
  OPENROAD_CMD="${OPENROAD_CMD%/}/bin/openroad"
fi

"$OPENROAD_CMD" -exit -no_init "$TCL_SCRIPT"
