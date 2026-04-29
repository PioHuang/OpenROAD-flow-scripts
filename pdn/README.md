# PDN Prototype

This directory contains a clean, standalone prototype for floorplan-stage PDN
modeling.

The code is intentionally split into four small concepts:

- `pdn/spec.hpp`: user-facing problem description
- `pdn/network.hpp`: discretized multilayer resistor graph
- `pdn/builder.hpp` + `src/builder.cpp`: converts the floorplan-stage problem
  into a graph
- `pdn/solver.hpp` + `src/solver.cpp`: solves the DC conductance system

## Modeling choices

This prototype uses the abstraction we discussed for early-stage IR analysis:

- each metal layer is represented by its own lattice
- in-layer wires become resistive edges
- vias become inter-layer resistive edges
- voltage sources fix node voltages on a chosen source layer
- hard loads are point-current injections
- soft logic can be modeled as distributed current over a region on the
  observation layer

The observation layer can use a finer pitch than the rest of the PDN through
`PdnProblem::observation_pitch_um`. That gives you a clean hook for a
floorplan-stage "bottom stable layer + abstract local access" workflow without
 forcing explicit followpin rails into the first implementation.

## Input schema

The solver now reads JSON input instead of using a hardcoded demo. It supports
two formats:

- **Direct PDN schema** (`examples/example_problem.json` style)
- **ORFS mempool manifest schema** (`research/mempool.json` style), mapped into
  the direct schema internally

Direct schema fields:

- `core_um`: `[llx, lly, urx, ury]`
- `observation_layer`: layer name whose voltage map you care about
- `observation_pitch_um`: optional refined lattice pitch for that layer
- `layers[]`: metal layer definitions
- `via_connections[]`: inter-layer resistive links
- `voltage_sources[]`: fixed-voltage source regions
- `point_loads[]`: point current sinks, useful for hard macros
- `distributed_loads[]`: region current sinks, useful for soft modules

See `examples/example_problem.json` for a complete runnable direct-schema
example.

For `research/mempool.json` compatibility, the loader consumes:

- `layout.core`
- `tech.set_rc_tcl`
- `pdn_vias_file`
- `psm_vsrc_boxes_file`
- `report_power_instances_tsv`
- `instance_geom_tsv`
- `groups`

and builds equivalent `PdnProblem` layers, vias, sources, and loads.

## Build

```bash
make
```

## Run the demo

```bash
make run
```

This uses `../research/mempool.json` by default when present, otherwise
`examples/example_problem.json`.

To run a different case:

```bash
./build/pdn_demo path/to/problem.json
```

## Dump outputs for GUI

Every run now dumps visualization artifacts to `out/latest` by default:

- `nodes.tsv`: solved voltage per node
- `edges.tsv`: graph edges
- `layers.tsv`: lattice summary
- `voltage_sources.tsv`: source regions
- `point_loads.tsv`: hard-like point loads
- `distributed_loads.tsv`: soft-like region loads
- `meta.tsv`: core bounds and solve statistics

Override output directory:

```bash
./build/pdn_demo ../research/mempool.json --dump-dir out/my_case
```

## Remote server GUI

Serve the dashboard and dumped TSVs:

```bash
python3 gui/serve.py --host 0.0.0.0 --port 8000
```

Open in browser:

- `http://<server-ip>:8000/gui/index.html`

The GUI overlays:

- node voltages (colored dots)
- graph connectivity (edge preview)
- distributed loads (cyan boxes)
- point loads (magenta crosses)
- voltage source regions (yellow boxes)
