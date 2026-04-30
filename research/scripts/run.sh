#!/usr/bin/env bash
set -euo pipefail

# One-shot runner for ORFS design flow + research prerequisites.
# Defaults:
#   platform=nangate45, design=mempool_group, variant=base
#
# Examples:
#   bash research/scripts/run.sh
#   bash research/scripts/run.sh --design black_parrot --manifest research/black_parrot.json

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FLOW_DIR="${REPO_ROOT}/flow"

PLATFORM="nangate45"
DESIGN="mempool_group"
BASE_VARIANT="base"
MANIFEST="research/mempool.json"
START_STAGE="1"
STOP_STAGE="8"
USE_RTLMP_SOFT_GUIDANCE=0
SOFT_GUIDANCE_WEIGHT="0.6"
SOFT_GUIDANCE_STAGE="gp"

stage_to_num() {
  local s="${1,,}"
  case "${s}" in
    1|synth) echo 1 ;;
    2|checkpoints|floorplan_checkpoints) echo 2 ;;
    3|rtlmp|rtlmp_extract) echo 3 ;;
    4|floorplan_place|place|placement) echo 4 ;;
    5|placement_viz|placepng) echo 5 ;;
    6|reports|power_geom|extract) echo 6 ;;
    7|normalize|membership) echo 7 ;;
    8|verify|manifest_verify) echo 8 ;;
    *) return 1 ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) PLATFORM="$2"; shift 2 ;;
    --design) DESIGN="$2"; shift 2 ;;
    --base-variant) BASE_VARIANT="$2"; shift 2 ;;
    --manifest) MANIFEST="$2"; shift 2 ;;
    --start-stage) START_STAGE="$2"; shift 2 ;;
    --stop-stage) STOP_STAGE="$2"; shift 2 ;;
    --use-rtlmp-soft-guidance) USE_RTLMP_SOFT_GUIDANCE=1; shift 1 ;;
    --soft-guidance-weight) SOFT_GUIDANCE_WEIGHT="$2"; shift 2 ;;
    --soft-guidance-stage) SOFT_GUIDANCE_STAGE="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--platform P] [--design D] [--base-variant B] [--manifest path] [--start-stage S] [--stop-stage S]"
      echo "  --design: directory name under flow/designs/<platform>/ (e.g. black_parrot)."
      echo "  ORFS results/reports/objects use DESIGN_NICKNAME from that config.mk (e.g. bp), not DESIGN_NAME."
      echo "  All flow steps use FLOW_VARIANT=\${BASE_VARIANT} (default: base) only."
      echo "  --start-stage: stage number/name to resume from."
      echo "  --stop-stage: stage number/name to stop after (default: verify / 8)."
      echo "    1:synth 2:checkpoints 3:rtlmp 4:floorplan_place 5:placement_viz 6:reports(extract) 7:normalize 8:verify"
      echo "  --use-rtlmp-soft-guidance: generate GPL soft guidance from RTLMP reports and pass to do-place."
      echo "  --soft-guidance-weight: weight passed to global_placement -soft_guidance_weight (default: 0.6)."
      echo "  --soft-guidance-stage: where to apply guidance: skip_io|gp|both (default: skip_io)."
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if ! START_STAGE_NUM="$(stage_to_num "${START_STAGE}")"; then
  echo "ERROR: invalid --start-stage '${START_STAGE}'" >&2
  echo "Valid values: 1..8 or synth/checkpoints/rtlmp/floorplan_place/placement_viz/reports/extract/normalize/verify" >&2
  exit 2
fi
if ! STOP_STAGE_NUM="$(stage_to_num "${STOP_STAGE}")"; then
  echo "ERROR: invalid --stop-stage '${STOP_STAGE}'" >&2
  echo "Valid values: 1..8 or synth/checkpoints/rtlmp/floorplan_place/placement_viz/reports/extract/normalize/verify" >&2
  exit 2
fi
if (( START_STAGE_NUM > STOP_STAGE_NUM )); then
  echo "ERROR: --start-stage (${START_STAGE}) must be <= --stop-stage (${STOP_STAGE})" >&2
  exit 2
fi

case "${SOFT_GUIDANCE_STAGE}" in
  skip_io|gp|both) ;;
  *)
    echo "ERROR: invalid --soft-guidance-stage '${SOFT_GUIDANCE_STAGE}'" >&2
    echo "Valid values: skip_io | gp | both" >&2
    exit 2
    ;;
esac

DESIGN_CFG="./designs/${PLATFORM}/${DESIGN}/config.mk"
DESIGN_DIR="./designs/${PLATFORM}/${DESIGN}"

if [[ ! -f "${FLOW_DIR}/${DESIGN_CFG#./}" ]]; then
  echo "ERROR: missing design config: ${FLOW_DIR}/${DESIGN_CFG#./}" >&2
  exit 1
fi

# ORFS variables.mk: RESULTS_DIR/REPORTS_DIR/... use $(DESIGN_NICKNAME), not $(DESIGN_NAME).
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

REPORT_DIR="./reports/${PLATFORM}/${DESIGN_NICKNAME}/${BASE_VARIANT}"
RESULT_DIR="./results/${PLATFORM}/${DESIGN_NICKNAME}/${BASE_VARIANT}"
OBJ_DIR="./objects/${PLATFORM}/${DESIGN_NICKNAME}/${BASE_VARIANT}"

# Avoid conda libstdc++ conflicts if system copy exists.
if [[ -f /usr/lib/x86_64-linux-gnu/libstdc++.so.6 ]]; then
  export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6
fi

echo "[design] platform=${PLATFORM} design_dir=${DESIGN} design_nickname=${DESIGN_NICKNAME} variant=${BASE_VARIANT}"
echo "[resume] start-stage=${START_STAGE} (resolved=${START_STAGE_NUM})"
echo "[stop] stop-stage=${STOP_STAGE} (resolved=${STOP_STAGE_NUM})"

cd "${FLOW_DIR}"
if (( START_STAGE_NUM <= 1 && STOP_STAGE_NUM >= 1 )); then
  echo "[1/8] Start from synthesis (traditional ORFS entry)"
  make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-1_synth
else
  echo "[1/8] Skipped (outside requested stage range)"
fi

if (( START_STAGE_NUM <= 2 && STOP_STAGE_NUM >= 2 )); then
  echo "[2/8] Floorplan checkpoints"
  make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-2_1_floorplan
  make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-2_2_floorplan_macro
else
  echo "[2/8] Skipped (outside requested stage range)"
fi

if (( START_STAGE_NUM <= 3 && STOP_STAGE_NUM >= 3 )); then
  echo "[3/8] RTLMP extraction and cluster reports"
  if [[ -f "${DESIGN_DIR}/rtlmp_extract.tcl" ]]; then
    # Keep this non-interactive; some Tcl scripts end without `exit` and would leave
    # an `openroad>` prompt that blocks the shell script.
    openroad -exit "${DESIGN_DIR}/rtlmp_extract.tcl"
  else
    echo "WARN: ${DESIGN_DIR}/rtlmp_extract.tcl not found; skipping explicit RTLMP extract script."
  fi
  # Cluster CSV/PNG/report: design may omit report.py (e.g. black_parrot). Use shared script + CORE_AREA from config.mk.
  if [[ -f "${OBJ_DIR}/rtlmp_extract/root.fp.txt" ]]; then
    core_vals="$(
      grep -E '^[[:space:]]*export[[:space:]]+CORE_AREA[[:space:]]*=' "${FLOW_DIR}/${DESIGN_CFG#./}" |
        head -n1 |
        sed -E 's/^[[:space:]]*export[[:space:]]+CORE_AREA[[:space:]]*=[[:space:]]*//' |
        tr -s '[:space:]' ' ' |
        tr -d '\r'
    )"
    read -r CORE_LX CORE_LY CORE_UX CORE_UY <<< "${core_vals}"
    if [[ -z "${CORE_LX:-}" || -z "${CORE_LY:-}" || -z "${CORE_UX:-}" || -z "${CORE_UY:-}" ]]; then
      echo "WARN: could not parse CORE_AREA from config.mk; skipping rtlmp_clusters.* under ${REPORT_DIR}"
    elif [[ -f "${DESIGN_DIR}/report.py" ]]; then
      python3 "${DESIGN_DIR}/report.py" \
        --fp "${OBJ_DIR}/rtlmp_extract/root.fp.txt" \
        --out-csv "${REPORT_DIR}/rtlmp_clusters.csv" \
        --out-png "${REPORT_DIR}/rtlmp_clusters.png" \
        --out-report "${REPORT_DIR}/rtlmp_clusters_report.md" \
        --core-lx "${CORE_LX}" --core-ly "${CORE_LY}" --core-ux "${CORE_UX}" --core-uy "${CORE_UY}"
    else
      python3 "${REPO_ROOT}/research/scripts/rtlmp_fp_report.py" \
        --fp "${OBJ_DIR}/rtlmp_extract/root.fp.txt" \
        --out-csv "${REPORT_DIR}/rtlmp_clusters.csv" \
        --out-png "${REPORT_DIR}/rtlmp_clusters.png" \
        --out-report "${REPORT_DIR}/rtlmp_clusters_report.md" \
        --core-lx "${CORE_LX}" --core-ly "${CORE_LY}" --core-ux "${CORE_UX}" --core-uy "${CORE_UY}"
    fi
  else
    echo "WARN: ${OBJ_DIR}/rtlmp_extract/root.fp.txt missing; skipping cluster PNG/CSV report (enable RTLMP_DEBUG_FLOORPLAN / macro floorplan)."
  fi

  if (( USE_RTLMP_SOFT_GUIDANCE == 1 )); then
    guidance_file="${OBJ_DIR}/rtlmp_extract/gpl_soft_guidance.txt"
    if [[ -f "${REPORT_DIR}/rtlmp_instance_to_cluster.txt" && -f "${REPORT_DIR}/rtlmp_clusters.csv" ]]; then
      python3 "${REPO_ROOT}/research/scripts/build_gpl_soft_guidance.py" \
        --membership "${FLOW_DIR}/${REPORT_DIR#./}/rtlmp_instance_to_cluster.txt" \
        --clusters-csv "${FLOW_DIR}/${REPORT_DIR#./}/rtlmp_clusters.csv" \
        --out "${FLOW_DIR}/${guidance_file#./}"
    else
      echo "WARN: missing rtlmp_instance_to_cluster.txt or rtlmp_clusters.csv; cannot build GPL soft guidance file."
    fi
  fi
else
  echo "[3/8] Skipped (outside requested stage range)"
fi

if (( START_STAGE_NUM <= 4 && STOP_STAGE_NUM >= 4 )); then
  echo "[4/8] Finish floorplan + placement (${BASE_VARIANT})"
  make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-2_3_floorplan_tapcell
  make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-2_4_floorplan_pdn
  make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-2_floorplan
  make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-2_floorplan.sdc
  if (( USE_RTLMP_SOFT_GUIDANCE == 1 )); then
    guidance_file="${OBJ_DIR}/rtlmp_extract/gpl_soft_guidance.txt"
    if [[ -f "${guidance_file}" ]]; then
      base_gp_args="$(
        grep -E '^[[:space:]]*export[[:space:]]+GLOBAL_PLACEMENT_ARGS[[:space:]]*=' "${FLOW_DIR}/${DESIGN_CFG#./}" |
          head -n1 |
          sed -E 's/^[[:space:]]*export[[:space:]]+GLOBAL_PLACEMENT_ARGS[[:space:]]*=[[:space:]]*//' |
          tr -d '\r'
      )"
      sg_args="${base_gp_args} -soft_guidance_file ${guidance_file} -soft_guidance_weight ${SOFT_GUIDANCE_WEIGHT}"
      echo "[4/8] Using GPL soft guidance file: ${guidance_file} (stage=${SOFT_GUIDANCE_STAGE})"

      case "${SOFT_GUIDANCE_STAGE}" in
        both)
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" \
            GLOBAL_PLACEMENT_ARGS="${sg_args}" do-place
          ;;
        skip_io)
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" \
            GLOBAL_PLACEMENT_ARGS="${sg_args}" do-3_1_place_gp_skip_io
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_2_place_iop
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" \
            GLOBAL_PLACEMENT_ARGS="${base_gp_args}" do-3_3_place_gp
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_4_place_resized
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_5_place_dp
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_place
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_place.sdc
          ;;
        gp)
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" \
            GLOBAL_PLACEMENT_ARGS="${base_gp_args}" do-3_1_place_gp_skip_io
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_2_place_iop
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" \
            GLOBAL_PLACEMENT_ARGS="${sg_args}" do-3_3_place_gp
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_4_place_resized
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_5_place_dp
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_place
          make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-3_place.sdc
          ;;
      esac
    else
      echo "WARN: soft guidance requested but file missing: ${guidance_file}; running do-place without soft guidance."
      make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-place
    fi
  else
    make DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" do-place
  fi
else
  echo "[4/8] Skipped (outside requested stage range)"
fi

if (( START_STAGE_NUM <= 5 && STOP_STAGE_NUM >= 5 )); then
  echo "[5/8] Placement visualization"
  if [[ -f "${DESIGN_DIR}/placepng.py" && -f "${RESULT_DIR}/3_place.odb" ]]; then
    membership_path="${REPORT_DIR}/rtlmp_cluster_membership.txt"
    [[ -f "${membership_path}" ]] || membership_path="${REPORT_DIR}/rtlmp_instance_to_cluster.txt"
    plan_csv="${REPORT_DIR}/rtlmp_clusters.csv"
    if [[ -f "${membership_path}" && -f "${plan_csv}" ]]; then
      python3 "${DESIGN_DIR}/placepng.py" \
        --odb "${FLOW_DIR}/${RESULT_DIR#./}/3_place.odb" \
        --membership "${FLOW_DIR}/${membership_path#./}" \
        --plan-csv "${FLOW_DIR}/${plan_csv#./}" \
        --out "${FLOW_DIR}/${REPORT_DIR#./}/place.png"
    else
      echo "WARN: missing membership or plan-csv; skipping placepng."
    fi
  else
    echo "WARN: placepng.py or place ODB missing; skipping placement image."
  fi
else
  echo "[5/8] Skipped (outside requested stage range)"
fi

if (( START_STAGE_NUM <= 6 && STOP_STAGE_NUM >= 6 )); then
  echo "[6/8] Generate research-required reports and DB extracts"
  if [[ -f "${DESIGN_DIR}/report_power.tcl" ]]; then
    make run DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" \
      RUN_SCRIPT="${DESIGN_DIR}/report_power.tcl" \
      RUN_LOG_NAME_STEM=report_power
  else
    echo "WARN: ${DESIGN_DIR}/report_power.tcl not found; skipping power report."
  fi
  if [[ -f "${DESIGN_DIR}/dump_instance_geom.tcl" ]]; then
    make run DESIGN_CONFIG="${DESIGN_CFG}" FLOW_VARIANT="${BASE_VARIANT}" \
      RUN_SCRIPT="${DESIGN_DIR}/dump_instance_geom.tcl" \
      RUN_LOG_NAME_STEM=dump_instance_geom
  else
    echo "WARN: ${DESIGN_DIR}/dump_instance_geom.tcl not found; skipping instance geom report."
  fi

  # Regenerate ODB-derived floorplan/PDN artifacts referenced by research/*.json.
  floorplan_odb="${FLOW_DIR}/${RESULT_DIR#./}/2_floorplan.odb"
  floorplan_sdc="${FLOW_DIR}/${RESULT_DIR#./}/2_floorplan.sdc"
  floorplan_extract_out="${REPO_ROOT}/research/out/floorplan_db_extract"
  if [[ -f "${floorplan_odb}" && -f "${floorplan_sdc}" ]]; then
    bash "${REPO_ROOT}/research/scripts/extract_floorplan_db.sh" \
      "${floorplan_odb}" "${floorplan_sdc}" "${floorplan_extract_out}"
  else
    echo "WARN: missing 2_floorplan.odb or 2_floorplan.sdc; skipping floorplan_db_extract regeneration."
  fi

  # Regenerate research/out/pdn_tcl_physical.tsv (consumed by 1142PDN).
  python3 "${REPO_ROOT}/research/scripts/generate_pdn_tcl_physical.py" \
    "${REPO_ROOT}/${MANIFEST}"
else
  echo "[6/8] Skipped (outside requested stage range)"
fi

if (( START_STAGE_NUM <= 7 && STOP_STAGE_NUM >= 7 )); then
  echo "[7/8] Normalize membership filename expected by research JSON"
  if [[ -f "${REPORT_DIR}/rtlmp_instance_to_cluster.txt" ]]; then
    :
  elif [[ -f "${REPORT_DIR}/rtlmp_cluster_membership.txt" ]]; then
    cp "${REPORT_DIR}/rtlmp_cluster_membership.txt" \
       "${REPORT_DIR}/rtlmp_instance_to_cluster.txt"
  else
    echo "WARN: missing both rtlmp_instance_to_cluster.txt and rtlmp_cluster_membership.txt"
  fi
else
  echo "[7/8] Skipped (outside requested stage range)"
fi

if (( START_STAGE_NUM <= 8 && STOP_STAGE_NUM >= 8 )); then
  echo "[8/8] Verify files referenced by manifest (${MANIFEST})"
  manifest_abs="${REPO_ROOT}/${MANIFEST}"
  if [[ ! -f "${manifest_abs}" ]]; then
    echo "ERROR: manifest not found: ${manifest_abs}" >&2
    exit 1
  fi

  mapfile -t required < <(python3 - "$manifest_abs" <<'PY'
import json, sys
p = sys.argv[1]
with open(p) as f:
    j = json.load(f)
keys = [
    "psm_vsrc_file",
    "psm_vsrc_boxes_file",
    "pdn_vias_file",
    "pdn_shapes_file",
    "report_power_instances_tsv",
    "instance_geom_tsv",
    "groups",
    "rtl_fp",
]
for k in keys:
    v = j.get(k)
    if isinstance(v, str) and v.strip():
        print(v.strip())
PY
)
  required+=("research/out/pdn_tcl_physical.tsv")

  missing=0
  for rel in "${required[@]}"; do
    f="${REPO_ROOT}/${rel}"
    if [[ ! -f "$f" ]]; then
      echo "ERROR: manifest references missing file: ${rel}" >&2
      missing=1
    fi
  done
  [[ "${missing}" -eq 0 ]] || exit 1
else
  echo "[8/8] Skipped (outside requested stage range)"
fi

echo "Done. Flow artifacts are ready for: ./research/build/phys_load ${MANIFEST}"
