#!/usr/bin/env bash
# Export post-PDN ODB to DEF for 1142PDN (`pdn_geometry.def`).
# Uses ORFS Make target `2_4_floorplan_pdn.odb.def` (correct OPENROAD_EXE).
#
# Prerequisites: `2_4_floorplan_pdn.odb` already built (e.g. `make ... do-2_4_floorplan_pdn`).
#
# Examples:
#   bash research/scripts/export_pdn_def_for_1142pdn.sh
#   bash research/scripts/export_pdn_def_for_1142pdn.sh --design black_parrot
#   bash research/scripts/export_pdn_def_for_1142pdn.sh --out 1142PDN/out/custom.def
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FLOW_DIR="${REPO_ROOT}/flow"

PLATFORM="nangate45"
DESIGN="mempool_group"
BASE_VARIANT="base"
OUT_DEF=""

usage() {
  cat <<'EOF'
Export post-PDN ODB to DEF for 1142PDN (pdn_geometry.def).
Uses ORFS: make … 2_4_floorplan_pdn.odb.def (needs 2_4_floorplan_pdn.odb from do-2_4_floorplan_pdn).

Usage: export_pdn_def_for_1142pdn.sh [--platform P] [--design D] [--base-variant B] [--out PATH]
  Default output: <repo>/1142PDN/out/<design>_pdn.def (1142PDN working tree; matches pdn_geometry.def in manifests)
EOF
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) PLATFORM="$2"; shift 2 ;;
    --design) DESIGN="$2"; shift 2 ;;
    --base-variant) BASE_VARIANT="$2"; shift 2 ;;
    --out) OUT_DEF="$2"; shift 2 ;;
    -h|--help) usage 0 ;;
    *)
      echo "Unknown argument: $1" >&2
      usage 2
      ;;
  esac
done

DESIGN_CFG="./designs/${PLATFORM}/${DESIGN}/config.mk"
if [[ ! -f "${FLOW_DIR}/${DESIGN_CFG#./}" ]]; then
  echo "ERROR: missing design config: ${FLOW_DIR}/${DESIGN_CFG#./}" >&2
  exit 1
fi

DESIGN_NICKNAME="${DESIGN}"
if grep -qE '^[[:space:]]*export[[:space:]]+DESIGN_NICKNAME[[:space:]]*=' "${FLOW_DIR}/${DESIGN_CFG#./}"; then
  DESIGN_NICKNAME="$(
    grep -E '^[[:space:]]*export[[:space:]]+DESIGN_NICKNAME[[:space:]]*=' "${FLOW_DIR}/${DESIGN_CFG#./}" |
      head -n1 |
      sed -E 's/^[[:space:]]*export[[:space:]]+DESIGN_NICKNAME[[:space:]]*=[[:space:]]*//' |
      tr -d '\r' |
      sed -E 's/[[:space:]]+$//'
  )"
fi

RESULT_DIR="./results/${PLATFORM}/${DESIGN_NICKNAME}/${BASE_VARIANT}"
PDN_ODB="${RESULT_DIR}/2_4_floorplan_pdn.odb"
PDN_DEF_BASENAME="2_4_floorplan_pdn.odb.def"

if [[ -z "${OUT_DEF}" ]]; then
  OUT_DEF="${REPO_ROOT}/1142PDN/out/${DESIGN}_pdn.def"
elif [[ "${OUT_DEF}" != /* ]]; then
  OUT_DEF="${REPO_ROOT}/${OUT_DEF#./}"
fi

if [[ -f /usr/lib/x86_64-linux-gnu/libstdc++.so.6 ]]; then
  export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6
fi

cd "${FLOW_DIR}"

if [[ ! -f "${PDN_ODB}" ]]; then
  echo "ERROR: missing PDN ODB (run floorplan PDN first): ${FLOW_DIR}/${RESULT_DIR#./}/2_4_floorplan_pdn.odb" >&2
  echo "  e.g.  cd ${FLOW_DIR} && make DESIGN_CONFIG=${DESIGN_CFG} FLOW_VARIANT=${BASE_VARIANT} do-2_4_floorplan_pdn" >&2
  exit 1
fi

echo "[export_pdn_def] platform=${PLATFORM} design=${DESIGN} nickname=${DESIGN_NICKNAME} variant=${BASE_VARIANT}"
make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" "${PDN_DEF_BASENAME}"
mkdir -p "$(dirname "${OUT_DEF}")"
cp -f "${RESULT_DIR}/${PDN_DEF_BASENAME}" "${OUT_DEF}"
echo "[export_pdn_def] wrote ${OUT_DEF}"
