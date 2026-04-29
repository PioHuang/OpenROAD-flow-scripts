Research IR plot GUI
====================

1) Run phys_load once so research/out/ir_mesh_nodes.tsv exists. The same run writes
   research/out/pdn_tcl_physical.tsv (literal add_pdn_stripe rectangles + add_pdn_connect rows
   parsed from tech.pdn_tcl in the manifest).

2) From the OpenROAD-flow-scripts repo root:

   python3 research/gui/start_server.py

   Optional: --port 8080 --host 127.0.0.1

3) Open in a browser:

   http://localhost:8765/

4) Click "Mesh current", "Mesh voltage", "VDD sources", or "PDN model". Each request runs
   research/scripts/plot.py and returns a fresh PNG.

   "PDN Tcl physical" loads research/out/pdn_tcl_physical.tsv (no plot.py): one integrated
   canvas (all layers) plus one tile per metal layer (same die/core framing); connect pairs
   are listed below (Tcl has no via coordinates).

   "PDN model 3D" runs plot.py model3d and shows two Plotly panes (core grid vs macro grids) from
   research/out/ir_pdn_model_3d_{std,macro}.html — install plotly in the Python used by the server:
   pip install plotly

5) "Open PNG from disk" uses a file picker (offline / compare).

Manifest and mesh paths in the page are passed as query parameters to /api/render
(relative to repo root, same as plot.py CLI).
