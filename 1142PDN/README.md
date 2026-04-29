# 1142PDN

`1142PDN` is a clean VoltSpot-like steady-state IR model for OpenROAD-flow-scripts
floorplan-stage inputs.

It follows VoltSpot's main abstraction:

- build a regular virtual grid
- size that grid from the lowest retained PDN layer pitch (ignoring filtered fine layers such as followpins)
- merge multi-layer metal stack conductance into effective `rx` and `ry`
- map sources and loads onto virtual-grid nodes
- solve one sparse resistive grid for steady-state node voltages

This implementation is adapted to ORFS inputs such as `research/mempool.json`
and its referenced TSV exports.

## Inputs used from ORFS

- `layout.core`
- `tech.set_rc_tcl`
- `research/out/pdn_tcl_physical.tsv`
- `psm_vsrc_boxes_file`
- `report_power_instances_tsv`
- `instance_geom_tsv`
- `groups`

## Current-load model

- hard macros: point loads split across PG pins, or instance center fallback
- soft logic: cluster bounding boxes with fractional knapsack current packing,
  then distributed to virtual-grid tiles by overlap area

## Run

```bash
make run
```

This reads `../research/mempool.json` by default and writes outputs to
`out/latest`.

## Outputs

- `nodes.tsv`: all node voltages and IR drop
- `edges.tsv`: virtual-grid graph
- `gridvol.tsv`: VoltSpot-like grid voltage report
- `hard.tsv`: hard-load placements
- `soft.tsv`: soft-cluster boxes
- `sources.tsv`: source regions
- `layers.tsv`: physical-to-virtual layer summary
- `meta.tsv`: run summary

## GUI

Serve the dashboard:

```bash
make gui
```

or choose another port:

```bash
make gui PORT=8011
```

Open:

- `http://<server-ip>:8010/gui/index.html`
- or if overridden, `http://<server-ip>:<PORT>/gui/index.html`

The GUI shows:

- final IR hotspot map
- virtual-grid graph
- source regions
- soft cluster boxes
- hard load points
