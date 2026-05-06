Build (run from repo root):

```
cmake -S research -B research/build && cmake --build research/build -j
```

Run:

```
research/build/phys_load research/mempool.json \
  [--mesh synth|odb|multilayer] [--pdn-csv <path>] [--ir-mode sum|max] \
  [--via-r <ohm>]
```

- `--mesh synth` (default): synthesize a uniform mesh on `layout.core` using
  the strap pitch/width inferred from `tech.pdn_tcl`. The outermost ring of
  mesh nodes is marked as the voltage source (ring).
- `--mesh odb`: read a PDN dump file (kind, layer, xlo, ylo, xhi, yhi) produced
  by `flow/scripts/dump_pdn_mesh.tcl` against a post-`2_4_floorplan_pdn` ODB.
  Mesh nodes come from real stripe intersections; ring nodes come from RING
  segments; strap widths and per-layer R are taken from the dump and
  `setRC.tcl`. All layers are collapsed into a single 2D grid.
- `--mesh multilayer`: build a 3D multi-layer mesh that mirrors the real PDN
  topology from `grid_strategy` (e.g. M1-M4-M7 for nangate45). Each layer has
  its own grid of nodes; inter-layer vias couple adjacent layers with
  conductance `1/via_r_ohm`. Voltage sources are placed at BTERM locations on
  the pin layer (e.g. metal7), falling back to RING segments or outermost
  nodes. Soft modules attach loads to the M1 (followpin) layer; hard macros
  attach to the M4 (lowest strap) layer. Requires `--pdn-csv`.
- `--via-r <ohm>`: inter-layer via resistance in ohms (default 0 = ideal
  short). Only meaningful with `--mesh multilayer`.
- `--ir-mode max` (default, paper): two-step approach. Step 1 solves the
  conductance-matrix system Gx=i (Jacobi-preconditioned CG) to obtain the
  voltage at every mesh node. Step 2 computes the local drop from each mesh
  node to the actual load (hard P/G pin or soft-module overlap region) using
  layer-specific R and width (multilayer) or `I * max(Rh, Rv)` (single-layer).
- `--ir-mode sum` (legacy, single-layer only): per-load IR drop =
  `I * (Rh + Rv)` via lumped Manhattan path from the load's nearest mesh node
  to the nearest ring node (no coupled mesh solve).

Generate a real PDN dump after `make 2_4_floorplan_pdn`:

```
openroad -no_init -exit -no_splash /dev/stdin <<'EOF'
source flow/scripts/dump_pdn_mesh.tcl
dump_pdn_mesh flow/results/nangate45/mempool_group/base/2_4_floorplan_pdn.odb \
              flow/results/nangate45/mempool_group/base/pdn_dump.csv
EOF
```

Then (single-layer, legacy):

```
research/build/phys_load research/mempool.json \
  --mesh odb \
  --pdn-csv flow/results/nangate45/mempool_group/base/pdn_dump.csv
```

Or (multi-layer M1-M4-M7):

```
research/build/phys_load research/mempool.json \
  --mesh multilayer \
  --pdn-csv flow/results/nangate45/mempool_group/base/pdn_dump.csv \
  --via-r 0.5
```

Outputs land in `<manifest_dir>/out/`:

- `ir_hard_macros.csv`   - per hard-macro PG pin, assigned current and
  estimated pin voltage (V_src - I * R from nearest ring node).
- `ir_soft_modules.csv`  - per RTLMP soft cluster, fractional-knapsack
  worst-case current and minimum tile voltage under the cluster box.
- `ir_mesh_nodes.csv`    - per mesh node, aggregated load current and the
  paper-style lumped voltage. Includes a `layer` column (e.g. `metal1`,
  `metal4`, `metal7`) for multi-layer meshes; empty for single-layer.
  Also includes `is_vsrc` (1 for voltage-source/BTERM/ring nodes, 0 otherwise).
- `ir_via_connections.csv` - inter-layer via connections from the multi-layer
  mesh adjacency list. Columns: `node_a layer_a x_a_um y_a_um node_b layer_b
  x_b_um y_b_um g_siemens`. Empty for single-layer meshes.
- `ir_pdn_stripes.csv`   - copy of the PDN dump segments (stripes, rings,
  followpins, BTERMs) colocated with the other CSVs. Same format as the
  `dump_pdn_mesh.tcl` output. Empty when `--mesh synth` is used.

## Visualization

Two optional post-processors turn the three CSVs into images / an interactive
viewer. Neither runs OpenROAD; both read only `<manifest_dir>/out/*.csv` and
the manifest (`layout.core` + `rtl_fp` cluster boxes), so `phys_load` and the
ORFS `finish` stage are left untouched.

Install Python deps once:

```
pip install -r research/requirements.txt
```

### Static PNG heatmaps (matplotlib)

```
python research/scripts/plot_ir.py \
  --manifest research/mempool.json \
  --out-dir  research/out \
  [--layer metal1]
```

Use `--layer` to select which layer is shown in the heatmaps (default: first
layer in the CSV, typically `metal1`). For single-layer meshes the flag is
ignored.

Writes five PNGs beside the CSVs:

- `ir_mesh_voltage.png`   mesh voltage heatmap (V) with core/die frames,
  voltage-source markers (red diamonds), and via location markers.
- `ir_mesh_current.png`   mesh total-current heatmap (A) with voltage-source
  and via markers.
- `ir_hard_pins.png`      voltage underlay + hard-macro PG pin scatter,
  marker size proportional to `imax_A`, color = `vpin_est_V`.
- `ir_soft_clusters.png`  voltage underlay + RTLMP soft-cluster rectangles
  colored by `vmin_tile_V` (boxes looked up from `rtl_fp`).
- `ir_layer_drops.png`    per-layer IR drop (mV) subplots (one per metal
  layer) with voltage-source and via markers, shared color scale.

### Interactive web viewers (Plotly.js, single file)

```
python research/scripts/pack_viewer.py \
  --manifest research/mempool.json \
  --out-dir  research/out \
  [--mode 2d|3d|both]
# open research/out/ir_viewer.html    (2D) or
#      research/out/ir_viewer_3d.html (3D) in any browser
```

The `--mode` flag controls which viewer(s) to generate (default: `both`).

#### 2D viewer (`ir_viewer.html`)

Single self-contained HTML file with the CSV data inlined as JSON and
Plotly.js pulled from a public CDN. Features:

- Mesh field switch: voltage (V), IR drop (mV), current total / hard / soft.
- Colormap + reverse-scale selector.
- Overlay toggles: voltage-source markers (red diamonds), hard-macro PG pins,
  soft-cluster boxes, core / die frames.
- Threshold slider to highlight only offending cells.
- Hover shows `(ix, iy, x, y, V, drop_mV, I_total, I_hard, I_soft)`.
- Layer selector for multi-layer meshes (defaults to the bottom layer).
- "Download PNG" button saves the current view.

#### 3D PDN viewer (`ir_viewer_3d.html`)

Interactive 3D visualization of the multi-layer PDN mesh. Features:

- Each metal layer rendered as a colored surface at a distinct Z-height,
  with IR drop / voltage / current color mapping.
- Voltage source (BTERM/ring) positions shown as 3D diamond markers.
- Inter-layer via connections drawn as vertical lines between layers.
- PDN stripe geometry (stripes, rings, followpins) as 3D line outlines.
- Hard-macro PG pin scatter on the appropriate layer.
- Sidebar controls: field selector, colormap, per-layer visibility toggles,
  overlay toggles, surface opacity slider, Z-spacing slider.
- Full 3D orbit/rotate/pan/zoom via Plotly.js built-in camera controls.
- "Download PNG" button saves the current 3D view.

Both viewers are completely decoupled from the OpenROAD GUI and work from
`file://` without any local server.
