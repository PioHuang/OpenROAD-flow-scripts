#!/usr/bin/env python3
"""Visualize DREAMPlace placement results from a DEF file.

Parses the DEF output of DREAMPlace (or any LEF/DEF placer) and renders:
  - Standard cells as scattered dots (color-coded by density)
  - Macro blocks (SRAMs, etc.) as filled rectangles
  - Die boundary outline

Usage:
    python3 plot_placement.py --def-input <placed.def> \
                              --lef-dir <platform_lef_dir> \
                              [--output <output.png>] \
                              [--dpi 300] [--figsize 12]
"""

import argparse
import os
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.collections import PatchCollection
import numpy as np


def parse_lef_sizes(lef_dir: str) -> dict[str, tuple[float, float]]:
    """Parse all LEF files in *lef_dir* and return {macro_name: (width_um, height_um)}."""
    sizes: dict[str, tuple[float, float]] = {}
    lef_dir = Path(lef_dir)
    if not lef_dir.is_dir():
        return sizes

    for lef_file in sorted(lef_dir.glob("*.lef")):
        current_macro = None
        with open(lef_file, "r") as f:
            for line in f:
                tokens = line.split()
                if not tokens:
                    continue
                if tokens[0] == "MACRO" and len(tokens) >= 2:
                    current_macro = tokens[1]
                elif tokens[0] == "SIZE" and current_macro:
                    w = float(tokens[1])
                    h = float(tokens[3])
                    sizes[current_macro] = (w, h)
                elif tokens[0] == "END" and current_macro and len(tokens) >= 2:
                    if tokens[1] == current_macro:
                        current_macro = None
    return sizes


def parse_def(def_path: str):
    """Parse a DEF file and return (dbu_per_um, die_box, placed_cells, fixed_cells).

    die_box : (x0, y0, x1, y1) in dbu
    placed_cells : list of (x_dbu, y_dbu, cell_type)
    fixed_cells  : list of (x_dbu, y_dbu, cell_type)
    """
    dbu_per_um = 1000  # default
    die_box = (0, 0, 0, 0)
    placed: list[tuple[int, int, str]] = []
    fixed: list[tuple[int, int, str]] = []

    re_diearea = re.compile(
        r"DIEAREA\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)"
    )
    re_units = re.compile(r"UNITS\s+DISTANCE\s+MICRONS\s+(\d+)")

    in_components = False
    pending_cell_type: str | None = None

    with open(def_path, "r") as f:
        for line in f:
            stripped = line.strip()

            m = re_units.search(stripped)
            if m:
                dbu_per_um = int(m.group(1))
                continue

            m = re_diearea.search(stripped)
            if m:
                die_box = (int(m.group(1)), int(m.group(2)),
                           int(m.group(3)), int(m.group(4)))
                continue

            if stripped.startswith("COMPONENTS "):
                in_components = True
                continue
            if stripped == "END COMPONENTS":
                in_components = False
                continue

            if not in_components:
                continue

            if stripped.startswith("- "):
                parts = stripped.split()
                if len(parts) >= 3:
                    pending_cell_type = parts[2]
                continue

            if pending_cell_type is not None and "+ PLACED" in stripped:
                m2 = re.search(r"\(\s*(-?\d+)\s+(-?\d+)\s*\)", stripped)
                if m2:
                    placed.append((int(m2.group(1)), int(m2.group(2)),
                                   pending_cell_type))
                pending_cell_type = None
                continue

            if pending_cell_type is not None and "+ FIXED" in stripped:
                m2 = re.search(r"\(\s*(-?\d+)\s+(-?\d+)\s*\)", stripped)
                if m2:
                    fixed.append((int(m2.group(1)), int(m2.group(2)),
                                  pending_cell_type))
                pending_cell_type = None
                continue

    return dbu_per_um, die_box, placed, fixed


MACRO_PREFIXES = ("fakeram", "SRAM", "sram", "RAM", "ram")


def is_macro(cell_type: str) -> bool:
    return any(cell_type.startswith(p) for p in MACRO_PREFIXES)


def plot_placement(
    def_path: str,
    lef_dir: str,
    output_path: str,
    dpi: int = 300,
    figsize: float = 12.0,
):
    print(f"Parsing LEF files from {lef_dir} ...")
    lef_sizes = parse_lef_sizes(lef_dir)
    print(f"  Found {len(lef_sizes)} cell/macro definitions")

    print(f"Parsing DEF file {def_path} ...")
    dbu_per_um, die_box, placed_cells, fixed_cells = parse_def(def_path)
    print(f"  DBU/um     : {dbu_per_um}")
    print(f"  Die area   : ({die_box[0]}, {die_box[1]}) - ({die_box[2]}, {die_box[3]}) dbu")
    print(f"  Placed std : {len(placed_cells)}")
    print(f"  Fixed cells: {len(fixed_cells)}")

    scale = 1.0 / dbu_per_um

    die_x0 = die_box[0] * scale
    die_y0 = die_box[1] * scale
    die_x1 = die_box[2] * scale
    die_y1 = die_box[3] * scale
    die_w = die_x1 - die_x0
    die_h = die_y1 - die_y0

    aspect = die_h / die_w if die_w > 0 else 1.0
    fig, ax = plt.subplots(
        figsize=(figsize, figsize * aspect),
        facecolor="#1a1a2e",
    )
    ax.set_facecolor("#1a1a2e")

    ax.add_patch(mpatches.Rectangle(
        (die_x0, die_y0), die_w, die_h,
        linewidth=1.5, edgecolor="#e0e0e0", facecolor="#2d2d44",
    ))

    # --- Macros (FIXED cells that are actual macro blocks) ---
    macro_patches = []
    tap_x, tap_y = [], []

    for x_dbu, y_dbu, ctype in fixed_cells:
        x_um = x_dbu * scale
        y_um = y_dbu * scale

        if ctype in lef_sizes and is_macro(ctype):
            w_um, h_um = lef_sizes[ctype]
            macro_patches.append(
                mpatches.Rectangle((x_um, y_um), w_um, h_um)
            )
        else:
            tap_x.append(x_um)
            tap_y.append(y_um)

    if macro_patches:
        pc = PatchCollection(
            macro_patches,
            facecolor="#ff4444",
            edgecolor="#cc0000",
            linewidth=0.3,
            alpha=0.85,
            zorder=3,
        )
        ax.add_collection(pc)
        print(f"  Drawn macros: {len(macro_patches)}")

    # --- Standard cells (PLACED) ---
    if placed_cells:
        xs = np.array([c[0] for c in placed_cells], dtype=np.float64) * scale
        ys = np.array([c[1] for c in placed_cells], dtype=np.float64) * scale

        pixels_per_um = figsize * dpi / max(die_w, die_h)
        point_area = max(0.3, pixels_per_um ** 2 * 0.15)
        ax.scatter(
            xs, ys,
            s=point_area,
            c="#9580ff",
            alpha=0.8,
            edgecolors="none",
            rasterized=True,
            zorder=2,
        )

    ax.set_xlim(die_x0 - die_w * 0.02, die_x1 + die_w * 0.02)
    ax.set_ylim(die_y0 - die_h * 0.02, die_y1 + die_h * 0.02)
    ax.set_aspect("equal")
    ax.set_xlabel("X (μm)", color="#cccccc", fontsize=10)
    ax.set_ylabel("Y (μm)", color="#cccccc", fontsize=10)
    ax.tick_params(colors="#999999", labelsize=8)
    for spine in ax.spines.values():
        spine.set_color("#555555")

    design_name = Path(def_path).stem
    ax.set_title(
        f"DREAMPlace Placement — {design_name}",
        color="#e0e0e0", fontsize=13, fontweight="bold", pad=12,
    )

    legend_handles = [
        mpatches.Patch(facecolor="#7b68ee", edgecolor="none", alpha=0.7,
                       label=f"Std cells ({len(placed_cells):,})"),
    ]
    if macro_patches:
        legend_handles.append(
            mpatches.Patch(facecolor="#ff4444", edgecolor="#cc0000",
                           label=f"Macros ({len(macro_patches):,})"),
        )
    ax.legend(
        handles=legend_handles, loc="upper right",
        fontsize=8, framealpha=0.7,
        facecolor="#2d2d44", edgecolor="#555555", labelcolor="#e0e0e0",
    )

    plt.tight_layout()
    plt.savefig(output_path, dpi=dpi, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"\nSaved placement image to: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Visualize DREAMPlace placement result from DEF file"
    )
    parser.add_argument(
        "--def-input", required=True,
        help="Path to the placed DEF file (e.g. *.gp.def from DREAMPlace)",
    )
    parser.add_argument(
        "--lef-dir", required=True,
        help="Directory containing LEF files (platform LEFs for cell/macro sizes)",
    )
    parser.add_argument(
        "--output", default=None,
        help="Output image path (default: <def_stem>_placement.png next to DEF)",
    )
    parser.add_argument(
        "--dpi", type=int, default=300,
        help="Image resolution (default: 300)",
    )
    parser.add_argument(
        "--figsize", type=float, default=12.0,
        help="Figure width in inches (default: 12)",
    )
    args = parser.parse_args()

    if not os.path.isfile(args.def_input):
        print(f"ERROR: DEF file not found: {args.def_input}", file=sys.stderr)
        if "$" in args.def_input or args.def_input.startswith("/dreamplace"):
            print("  Hint: did you forget to 'export RESULTS_DIR=...'?",
                  file=sys.stderr)
        sys.exit(1)

    if not os.path.isdir(args.lef_dir):
        print(f"ERROR: LEF directory not found: {args.lef_dir}", file=sys.stderr)
        sys.exit(1)

    if args.output is None:
        stem = Path(args.def_input).stem
        args.output = str(Path(args.def_input).parent / f"{stem}_placement.png")

    plot_placement(
        def_path=args.def_input,
        lef_dir=args.lef_dir,
        output_path=args.output,
        dpi=args.dpi,
        figsize=args.figsize,
    )


if __name__ == "__main__":
    main()
