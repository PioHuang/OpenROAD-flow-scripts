#!/usr/bin/env bash
#
# run_dreamplace_flow.sh
#
# Orchestrates the full OpenROAD -> DREAMPlace -> OpenROAD pipeline:
#   1. Export DEF + Verilog from OpenROAD floorplan
#   2. Inject RTLMP cluster fence regions into DEF
#   3. Run DREAMPlace placement with fence constraints
#   4. Import DREAMPlace results back into OpenROAD
#
# Prerequisites:
#   - OpenROAD floorplan must be completed (2_floorplan.odb exists)
#   - RTLMP clustering reports must exist (rtlmp_clusters.csv, rtlmp_instance_to_cluster.txt)
#   - DREAMPlace conda environment must be set up
#
# Usage:
#   bash run_dreamplace_flow.sh [--continue-openroad]
#
#   --continue-openroad  Continue OpenROAD flow: 3_2 IO placement,
#                        skip 3_3 global placement (DREAMPlace replaces it),
#                        then 3_4 resize, 3_5 detail place, CTS, route, finish

set -euo pipefail

# ── Paths ──────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORFS_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
FLOW_DIR="${ORFS_DIR}/flow"

PLATFORM="nangate45"
DESIGN="mempool_group"
VARIANT="base"

RESULTS_DIR="${FLOW_DIR}/results/${PLATFORM}/${DESIGN}/${VARIANT}"
REPORTS_DIR="${FLOW_DIR}/reports/${PLATFORM}/${DESIGN}/${VARIANT}"

OPENROAD_EXE="${ORFS_DIR}/tools/install/OpenROAD/bin/openroad"
DREAMPLACE_DIR="/home/yenchulo/DREAMPlace"
CONDA_SH="/home/yenchulo/anaconda3/etc/profile.d/conda.sh"

# ── Validate prerequisites ────────────────────────────────────────────────────
echo "============================================================"
echo " OpenROAD -> DREAMPlace -> OpenROAD Flow"
echo "============================================================"

for f in \
    "${RESULTS_DIR}/2_floorplan.odb" \
    "${RESULTS_DIR}/2_floorplan.sdc" \
    "${REPORTS_DIR}/rtlmp_clusters.csv" \
    "${REPORTS_DIR}/rtlmp_instance_to_cluster.txt" \
    "${OPENROAD_EXE}" \
    "${DREAMPLACE_DIR}/dreamplace/Placer.py"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: Required file not found: $f"
        exit 1
    fi
done

echo "All prerequisites found."
echo ""

# ── Step 1: Export DEF + Verilog from OpenROAD ─────────────────────────────────
echo "──────────────────────────────────────────────────────────────"
echo " Step 1: Exporting DEF + Verilog from OpenROAD floorplan"
echo "──────────────────────────────────────────────────────────────"

export RESULTS_DIR
"${OPENROAD_EXE}" -no_splash -no_init "${SCRIPT_DIR}/export_for_dreamplace.tcl"

if [[ ! -f "${RESULTS_DIR}/2_floorplan_for_dp.def" ]]; then
    echo "ERROR: DEF export failed"
    exit 1
fi
echo "Step 1 complete."
echo ""

# ── Step 2: Inject fence regions into DEF ──────────────────────────────────────
echo "──────────────────────────────────────────────────────────────"
echo " Step 2: Injecting RTLMP cluster fence regions into DEF"
echo "──────────────────────────────────────────────────────────────"

python3 "${SCRIPT_DIR}/inject_fence_regions.py" \
    --clusters-csv  "${REPORTS_DIR}/rtlmp_clusters.csv" \
    --instance-map  "${REPORTS_DIR}/rtlmp_instance_to_cluster.txt" \
    --def-input     "${RESULTS_DIR}/2_floorplan_for_dp.def" \
    --def-output    "${RESULTS_DIR}/2_floorplan_fenced.def"

if [[ ! -f "${RESULTS_DIR}/2_floorplan_fenced.def" ]]; then
    echo "ERROR: Fence injection failed"
    exit 1
fi
echo "Step 2 complete."
echo ""

# ── Step 3: Run DREAMPlace ─────────────────────────────────────────────────────
echo "──────────────────────────────────────────────────────────────"
echo " Step 3: Running DREAMPlace placement"
echo "──────────────────────────────────────────────────────────────"

DP_RESULTS_DIR="${RESULTS_DIR}/dreamplace_results"
mkdir -p "${DP_RESULTS_DIR}"

# DREAMPlace derives design name from verilog_input basename
DP_DESIGN_NAME="2_floorplan_for_dp"
DP_OUTPUT_DEF="${DP_RESULTS_DIR}/${DP_DESIGN_NAME}/${DP_DESIGN_NAME}.gp.def"

# Activate DREAMPlace conda environment and run
(
    source "${CONDA_SH}"
    conda activate DREAMPlace
    cd "${DREAMPLACE_DIR}/install"
    python dreamplace/Placer.py "${SCRIPT_DIR}/mempool_group_fenced.json"
)

if [[ ! -f "${DP_OUTPUT_DEF}" ]]; then
    echo "ERROR: DREAMPlace output not found at ${DP_OUTPUT_DEF}"
    echo "  Checking for alternative output locations..."
    find "${DP_RESULTS_DIR}" -name "*.gp.def" -type f 2>/dev/null
    exit 1
fi
echo "Step 3 complete."
echo "  DREAMPlace output: ${DP_OUTPUT_DEF}"
echo ""

# ── Step 3.5: Visualize placement ─────────────────────────────────────────────
echo "──────────────────────────────────────────────────────────────"
echo " Step 3.5: Generating placement visualization"
echo "──────────────────────────────────────────────────────────────"

LEF_DIR="${FLOW_DIR}/platforms/${PLATFORM}/lef"
PLACEMENT_PNG="${RESULTS_DIR}/dreamplace_placement.png"

python3 "${SCRIPT_DIR}/plot_placement.py" \
    --def-input "${DP_OUTPUT_DEF}" \
    --lef-dir "${LEF_DIR}" \
    --output "${PLACEMENT_PNG}"

echo ""

# ── Step 4: Import placement back into OpenROAD ───────────────────────────────
echo "──────────────────────────────────────────────────────────────"
echo " Step 4: Importing DREAMPlace results into OpenROAD ODB"
echo "──────────────────────────────────────────────────────────────"

export DREAMPLACE_DEF="${DP_OUTPUT_DEF}"
"${OPENROAD_EXE}" -no_splash -no_init "${SCRIPT_DIR}/import_dreamplace.tcl"

if [[ ! -f "${RESULTS_DIR}/3_1_place_gp_skip_io.odb" ]]; then
    echo "ERROR: Import failed - 3_1_place_gp_skip_io.odb not created"
    exit 1
fi
echo "Step 4 complete."
echo ""

# ── Step 5 (optional): Continue OpenROAD flow ─────────────────────────────────
if [[ "${1:-}" == "--continue-openroad" ]]; then
    echo "──────────────────────────────────────────────────────────────"
    echo " Step 5: Continuing OpenROAD flow"
    echo "   3_2 -> (skip 3_3) -> 3_4 -> 3_5 -> CTS -> Route -> Finish"
    echo "──────────────────────────────────────────────────────────────"
    cd "${FLOW_DIR}"
    DESIGN_CFG="./designs/${PLATFORM}/${DESIGN}/config.mk"

    echo ""
    echo "  [3_2] IO placement..."
    make do-3_2_place_iop DESIGN_CONFIG="${DESIGN_CFG}"

    echo ""
    echo "  [3_3] SKIPPED — copying 3_2 output as 3_3 (DREAMPlace already did GP)"
    cp "${RESULTS_DIR}/3_2_place_iop.odb" "${RESULTS_DIR}/3_3_place_gp.odb"

    echo ""
    echo "  [3_4] Resizing & buffering..."
    make do-3_4_place_resized DESIGN_CONFIG="${DESIGN_CFG}"

    echo ""
    echo "  [3_5] Detail placement..."
    make do-3_5_place_dp do-3_place do-3_place.sdc DESIGN_CONFIG="${DESIGN_CFG}"

    echo ""
    echo "  [4] CTS..."
    make do-cts DESIGN_CONFIG="${DESIGN_CFG}"

    echo ""
    echo "  [5] Routing..."
    make do-route DESIGN_CONFIG="${DESIGN_CFG}"

    echo ""
    echo "  [6] Finish..."
    make do-finish DESIGN_CONFIG="${DESIGN_CFG}"

    echo "Step 5 complete."
fi

echo ""
echo "============================================================"
echo " Pipeline complete!"
echo "============================================================"
echo "  Fenced DEF:       ${RESULTS_DIR}/2_floorplan_fenced.def"
echo "  DREAMPlace DEF:   ${DP_OUTPUT_DEF}"
echo "  Placement image:  ${PLACEMENT_PNG}"
echo "  Placement ODB:    ${RESULTS_DIR}/3_1_place_gp_skip_io.odb"
echo "  Placement SDC:    ${RESULTS_DIR}/3_1_place_gp_skip_io.sdc"
echo ""
echo "To continue in OpenROAD (skip 3_3, run 3_2 -> 3_4 -> 3_5 -> 4 -> 5 -> 6):"
echo "  bash ${SCRIPT_DIR}/run_dreamplace_flow.sh --continue-openroad"
echo ""
echo "Or manually:"
echo "  cd ${FLOW_DIR}"
echo "  DESIGN_CFG=./designs/${PLATFORM}/${DESIGN}/config.mk"
echo "  make do-3_2_place_iop DESIGN_CONFIG=\$DESIGN_CFG"
echo "  cp ${RESULTS_DIR}/3_2_place_iop.odb ${RESULTS_DIR}/3_3_place_gp.odb"
echo "  make do-3_4_place_resized do-3_5_place_dp do-3_place do-3_place.sdc DESIGN_CONFIG=\$DESIGN_CFG"
echo "  make do-cts do-route do-finish DESIGN_CONFIG=\$DESIGN_CFG"
