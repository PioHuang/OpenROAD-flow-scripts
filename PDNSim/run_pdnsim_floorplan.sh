#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <odb_file> <sdc_file> <out_dir> [net] [voltage] [source_type] [vsrc_file]"
  exit 1
fi

ODB_FILE="$1"
SDC_FILE="$2"
OUT_DIR="$3"
PDN_NET="${4:-VDD}"
PDN_VOLTAGE="${5:-1.1}"
SOURCE_TYPE="${6:-FULL}"
VSRC_FILE="${7:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TCL_SCRIPT="${SCRIPT_DIR}/run_pdnsim_floorplan.tcl"

export ODB_FILE SDC_FILE OUT_DIR PDN_NET PDN_VOLTAGE SOURCE_TYPE VSRC_FILE

SYS_LIBSTDCXX="/usr/lib/x86_64-linux-gnu/libstdc++.so.6"
if [[ -f "$SYS_LIBSTDCXX" ]]; then
  export LD_PRELOAD="${SYS_LIBSTDCXX}${LD_PRELOAD:+:$LD_PRELOAD}"
fi

OPENROAD_CMD="${OPENROAD_BIN:-openroad}"
if [[ -d "$OPENROAD_CMD" ]]; then
  OPENROAD_CMD="${OPENROAD_CMD%/}/bin/openroad"
fi

"$OPENROAD_CMD" -exit -no_init "$TCL_SCRIPT"
