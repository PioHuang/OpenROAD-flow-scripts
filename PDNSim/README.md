# PDNSim (Standalone Folder)

This folder is outside `research` and contains:

- runnable PDNSim floorplan flow scripts
- copied PDNSim source files for local editing
- a `Makefile` with `build` and `run` targets
- standalone PDN extractor build/run targets

## Files

- `run_pdnsim_floorplan.sh`: shell entrypoint
- `run_pdnsim_floorplan.tcl`: OpenROAD/PDNSim commands

## Build and Run

From this folder:

```bash
cd PDNSim
make build
make run
```

## ODB Extraction + PDN Extractor

First extract geometry from ODB/SDC into `PDNSim/out`:

```bash
make prep
```

Then build/run extractor:

```bash
make extract_build
make extract
```

Override inputs/outputs:

```bash
make extract SHAPES=./out/db_pdn_shapes.tsv \
             VIAS=./out/pdn_vias_VDD.tsv \
             NET=VDD EXTRACT_OUT=./out/pdn_extract_report.tsv
```

You can override run-time inputs:

```bash
make run ODB=../flow/results/nangate45/mempool_group/rtlmp/2_floorplan.odb \
         SDC=../flow/results/nangate45/mempool_group/rtlmp/2_floorplan.sdc \
         OUT=./out NET=VDD VOLT=1.1 SOURCE=STRAPS \
         VSRC=../flow/results/nangate45/mempool_group/rtlmp/Vsrc.loc
```

## Local Source Files (editable)

- `src/pdnsim.cpp`
- `src/ir_network.cpp`
- `src/ir_network.h`
- `src/ir_solver.cpp`
- `src/ir_solver.h`
- `include/psm/pdnsim.h`

## Outputs

Generated in `<out_dir>`:

- `<net>_conn_floorplan.rpt`
- `<net>_conn_strict.rpt`
- `<net>_analyze_err.rpt`
- `<net>_instance_voltage.rpt`
- `pdn_extract_report.tsv` (from `make extract`)
