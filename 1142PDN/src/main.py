#!/usr/bin/env python3
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from vpdn.orfs import load_problem
from vpdn.solve import dump, solve
from vpdn.voltspot import build_grid


def main() -> int:
    parser = argparse.ArgumentParser(description="Floorplan PDN IR model for ORFS inputs")
    parser.add_argument("--manifest", default="../research/mempool.json", help="ORFS manifest like research/mempool.json")
    parser.add_argument("--dump-dir", default="../out/latest", help="Output directory")
    parser.add_argument("--grid-intv", type=int, default=1, help="Legacy option; virtual-grid spacing now follows the lowest retained PDN layer pitch")
    parser.add_argument("--pad-r", type=float, default=10e-3, help="Source pad resistance (ohm)")
    parser.add_argument("--min-layer-pitch-um", type=float, default=5.0, help="Ignore finer layers than this")
    args = parser.parse_args()

    problem = load_problem(
        manifest_path=args.manifest,
        grid_intv=args.grid_intv,
        pad_r_ohm=args.pad_r,
        min_pitch_um=args.min_layer_pitch_um,
    )
    grid = build_grid(problem)
    sol = solve(problem, grid)
    dump(problem, grid, sol, Path(args.dump_dir))

    print("Steady-state PDN solved")
    print(f"  manifest: {args.manifest}")
    print(f"  design: {problem.name}")
    print(f"  layers: {len(problem.layers)}")
    print(f"  hard loads: {len(problem.hard)}")
    print(f"  soft clusters: {len(problem.soft)}")
    print(f"  virtual grid: {grid.rows} x {grid.cols}")
    print(f"  grid pitch (um): {problem.grid_pitch_um:.3f}")
    print(f"  rx_edge (ohm): {grid.rx_ohm:.6e}")
    print(f"  ry_edge (ohm): {grid.ry_ohm:.6e}")
    print(f"  min voltage (V): {min(sol.volt_v):.9f}")
    print(f"  max voltage (V): {max(sol.volt_v):.9f}")
    print(f"  max drop (%Vdd): {100.0 * (problem.vdd - min(sol.volt_v)) / max(problem.vdd, 1e-12):.6f}")
    print(f"  iterations: {sol.iters}")
    print(f"  residual: {sol.resid:.6e}")
    print(f"  dump dir: {args.dump_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
