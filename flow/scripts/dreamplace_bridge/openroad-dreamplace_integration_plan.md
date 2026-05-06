---
name: OpenROAD-DREAMPlace integration
overview: Integrate DREAMPlace as the placement engine between OpenROAD floorplan and CTS stages, using RTLMP cluster boundaries as fence regions to constrain standard cell placement.
todos:
  - id: export-def
    content: "Write export_for_dreamplace.tcl: export DEF + Verilog from OpenROAD 2_floorplan.odb"
    status: completed
  - id: inject-fence
    content: "Write inject_fence_regions.py: parse rtlmp_clusters.csv + rtlmp_instance_to_cluster.txt, inject REGIONS/GROUPS (TYPE FENCE) into DEF"
    status: completed
  - id: dreamplace-config
    content: "Write mempool_group_fenced.json: DREAMPlace config pointing to fenced DEF and correct LEF files"
    status: completed
  - id: import-result
    content: "Write import_dreamplace.tcl: load DREAMPlace output DEF placement into OpenROAD ODB, save as 3_place.odb"
    status: completed
  - id: master-script
    content: "Write run_dreamplace_flow.sh: orchestrate the full pipeline (export -> inject -> DREAMPlace -> import -> continue OpenROAD)"
    status: completed
  - id: test-run
    content: Test the full flow end-to-end on mempool_group/nangate45
    status: completed
isProject: false
---

# OpenROAD Floorplan -> DREAMPlace Placement -> OpenROAD Final Flow

## Overview

The goal is to replace OpenROAD's built-in placement (stages 3_1 through 3_5) with DREAMPlace, while enforcing RTLMP cluster boundaries as fence regions. The pipeline becomes:

```
OpenROAD floorplan (stages 1-2) --> Export DEF + cluster data
    --> Python bridge script (generate fenced DEF)
    --> DREAMPlace placement (with fence regions)
    --> Import placement back into OpenROAD
    --> Continue OpenROAD (CTS, routing, finishing: stages 4-6)
```

## Key Data Sources

- **RTLMP cluster boundaries**: [rtlmp_clusters.csv](OpenROAD-flow-scripts/flow/reports/nangate45/mempool_group/base/rtlmp_clusters.csv) -- CSV with `lx,ly,ux,uy` (in um) for each cluster
- **Instance-to-cluster mapping**: [rtlmp_instance_to_cluster.txt](OpenROAD-flow-scripts/flow/reports/nangate45/mempool_group/base/rtlmp_instance_to_cluster.txt) -- maps each standard cell instance to its cluster (format: `instance_name cluster_id cluster_name depth`)
- **Floorplan result**: `OpenROAD-flow-scripts/flow/results/nangate45/mempool_group/base/2_floorplan.odb` (OpenROAD database after floorplan)
- **DREAMPlace**: installed at `/home/yenchulo/DREAMPlace/`, with conda env `DREAMPlace` (Python 3.9)
- **DREAMPlace fence support**: reads DEF `REGIONS` (type FENCE) + `GROUPS` sections. Internally uses `node2fence_region_map`, `flat_region_boxes`, `regions` arrays (see [PlaceDB.py](DREAMPlace/dreamplace/PlaceDB.py) and [PlaceDB.cpp](DREAMPlace/dreamplace/ops/place_io/src/PlaceDB.cpp))

## Step-by-step Plan

### Step 1: Export DEF from OpenROAD after floorplan

Write a Tcl script (`export_for_dreamplace.tcl`) that:
- Loads `2_floorplan.odb` + `2_floorplan.sdc`
- Writes out a DEF file: `2_floorplan_for_dp.def`
- Writes out a Verilog netlist: `2_floorplan_for_dp.v` (DREAMPlace may need it for netlist connectivity if not fully in DEF)

This can be done with existing OpenROAD commands: `write_def` and `write_verilog`.

### Step 2: Python bridge script -- inject REGIONS/GROUPS into DEF

Write a Python script (`inject_fence_regions.py`) that:
1. Reads `rtlmp_clusters.csv` to get cluster names and bounding boxes (only rows where `kind == "cluster"`, skipping `point`, `hard_macro`, `io_boundary`)
2. Reads `rtlmp_instance_to_cluster.txt` to get the instance-to-cluster mapping
3. Reads the exported `2_floorplan_for_dp.def`
4. Injects DEF `REGIONS` and `GROUPS` sections before `END DESIGN`:

```
REGIONS <N> ;
  - cluster_0 ( lx_dbu ly_dbu ) ( ux_dbu uy_dbu ) + TYPE FENCE ;
  - cluster_1 ( lx_dbu ly_dbu ) ( ux_dbu uy_dbu ) + TYPE FENCE ;
  ...
END REGIONS

GROUPS <N> ;
  - cluster_0 inst1 inst2 inst3 ... + REGION cluster_0 ;
  - cluster_1 inst4 inst5 inst6 ... + REGION cluster_1 ;
  ...
END GROUPS
```

Coordinates must be in DEF database units (dbu). The CSV has both `x_dbu,y_dbu,w_dbu,h_dbu` and `lx,ly,ux,uy` (in um). We use the dbu columns directly, or convert `lx/ly/ux/uy` (um) by multiplying with the DEF database units per micron (typically 2000 for nangate45).

**Important**: Only include `kind == "cluster"` entries (38 clusters). Skip `point` (anchor-only), `hard_macro` (already fixed), and `io_boundary` entries.

### Step 3: Create DREAMPlace JSON config

Write a JSON config (`mempool_group_fenced.json`) for DREAMPlace:

```json
{
  "lef_input": [
    "<path>/NangateOpenCellLibrary.tech.lef",
    "<path>/NangateOpenCellLibrary.macro.mod.lef",
    "<path>/fakeram45_256x32.lef",
    "<path>/fakeram45_64x64.lef"
  ],
  "def_input": "<path>/2_floorplan_fenced.def",
  "verilog_input": "<path>/2_floorplan_for_dp.v",
  "gpu": 1,
  "num_bins_x": 512,
  "num_bins_y": 512,
  "global_place_stages": [
    {"num_bins_x": 512, "num_bins_y": 512, "iteration": 1000, 
     "learning_rate": 0.01, "wirelength": "weighted_average", "optimizer": "nesterov"}
  ],
  "target_density": 0.4,
  "density_weight": 8e-5,
  "gamma": 4.0,
  "random_seed": 1000,
  "enable_fillers": 1,
  "global_place_flag": 1,
  "legalize_flag": 1,
  "detailed_place_flag": 0,
  "stop_overflow": 0.1,
  "result_dir": "results",
  "num_threads": 8,
  "plot_flag": 1,
  "sol_file_format": "DEF"
}
```

Key: the `target_density` should match OpenROAD's `PLACE_DENSITY` (0.4). DREAMPlace will read the REGIONS/GROUPS from the DEF and automatically build `node2fence_region_map`, constraining cells within their fence regions.

### Step 4: Run DREAMPlace

```bash
conda activate DREAMPlace
cd /home/yenchulo/DREAMPlace
python dreamplace/Placer.py <path>/mempool_group_fenced.json
```

DREAMPlace will output a placed DEF file (`.gp.def`) in the results directory.

### Step 5: Import DREAMPlace result back into OpenROAD

Write a Tcl script (`import_dreamplace_result.tcl`) that:
1. Loads the original `2_floorplan.odb` (which has the complete OpenROAD database with PDN, tapcells, etc.)
2. Uses `read_def -placement <dreamplace_output.def>` or a custom approach to import only the cell placement coordinates from DREAMPlace into the existing ODB
3. Saves the result as `3_place.odb` (matching what OpenROAD's CTS stage expects)
4. Copies `2_floorplan.sdc` to `3_place.sdc`

**Alternative approach** (more robust): Instead of `read_def -placement`, write a Python/Tcl script that:
- Parses the DREAMPlace output DEF for COMPONENTS placement coordinates
- Loads the OpenROAD ODB
- Iterates over all COMPONENTS and updates each instance's x/y and placement status to PLACED
- Saves as `3_place.odb`

### Step 6: Continue OpenROAD from CTS onward

```bash
cd /home/yenchulo/OpenROAD-flow-scripts/flow
make cts route finish DESIGN_CONFIG=./designs/nangate45/mempool_group/config.mk
```

Since `3_place.odb` and `3_place.sdc` exist, OpenROAD's CTS (stage 4) will pick them up and continue normally.

## File Structure

All new scripts will live under a single directory:

```
OpenROAD-flow-scripts/flow/scripts/dreamplace_bridge/
  inject_fence_regions.py    # Step 2: inject REGIONS/GROUPS into DEF
  export_for_dreamplace.tcl  # Step 1: export DEF + Verilog from OpenROAD
  import_dreamplace.tcl      # Step 5: import DREAMPlace results back
  mempool_group_fenced.json  # Step 3: DREAMPlace config
  run_dreamplace_flow.sh     # Master script orchestrating all steps
```

## Risks and Considerations

- **Row alignment**: DREAMPlace's legalized output should respect the row structure from the DEF. Since we export the DEF from OpenROAD (which includes ROWS), DREAMPlace will use those rows for legalization.
- **Fixed cells**: Hard macros in the DEF have `FIXED` status; DREAMPlace respects this and will not move them.
- **Filler cells**: OpenROAD adds tapcells and endcaps during floorplan. These are FIXED in the DEF and will be preserved.
- **PDN**: Power delivery network structures exist in the ODB but not in the DEF. By importing placement back into the original ODB, PDN is preserved.
- **Timing**: DREAMPlace does not perform timing-driven resizing. The OpenROAD flow has a resize step (3_4_place_resized) between global and detailed placement. You may want to run OpenROAD's `resize` step after importing DREAMPlace results, before CTS.
- **Cluster coverage**: Some instances may map to `point`-type clusters (which have no real area). These should be left unconstrained (no fence region) -- they will be placed freely by DREAMPlace in the non-fenced area.
