#!/usr/bin/env python3
"""
Research plotting entry point (phys_load outputs, etc.).

Run from OpenROAD-flow-scripts repo root:

  python3 research/scripts/plot.py              # same as: plot.py current
  python3 research/scripts/plot.py current      # mesh current: soft / hard / total + pins
  python3 research/scripts/plot.py voltage      # solved node voltages + pins
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parent.parent.parent


def load_core_rect(root: Path) -> tuple[float, float, float, float] | None:
    mp = root / "research/mempool.json"
    if not mp.is_file():
        return None
    try:
        with mp.open() as f:
            c = json.load(f)["layout"]["core"]
        return float(c[0]), float(c[1]), float(c[2]), float(c[3])
    except Exception:
        return None


def add_core_patches(axes, core: tuple[float, float, float, float] | None) -> None:
    if core is None:
        return
    lx, ly, ux, uy = core
    for ax in axes:
        ax.add_patch(
            mpatches.Rectangle(
                (lx, ly),
                ux - lx,
                uy - ly,
                fill=False,
                linestyle="--",
                linewidth=0.9,
                edgecolor="0.4",
                zorder=2,
            )
        )


def load_pins(tsv: Path) -> tuple[np.ndarray, np.ndarray]:
    if not tsv.is_file():
        return np.array([]), np.array([])
    px, py = [], []
    with tsv.open(newline="") as f:
        r = csv.DictReader(f, delimiter="\t")
        for row in r:
            px.append(float(row["pin_x_um"]))
            py.append(float(row["pin_y_um"]))
    return np.array(px), np.array(py)


def draw_mesh_edges(ax, ix, iy, x, y, alpha=0.3, lw=0.3):
    pos = {(int(a), int(b)): (float(xp), float(yp)) for a, b, xp, yp in zip(ix, iy, x, y)}
    for (i, j), (xp, yp) in pos.items():
        if (i + 1, j) in pos:
            x2, y2 = pos[(i + 1, j)]
            ax.plot([xp, x2], [yp, y2], "k-", lw=lw, alpha=alpha, zorder=1)
        if (i, j + 1) in pos:
            x2, y2 = pos[(i, j + 1)]
            ax.plot([xp, x2], [yp, y2], "k-", lw=lw, alpha=alpha, zorder=1)


def load_mesh_current(tsv: Path):
    ix, iy, x, y = [], [], [], []
    i_soft, i_hard = [], []
    with tsv.open(newline="") as f:
        r = csv.DictReader(f, delimiter="\t")
        for row in r:
            ix.append(int(row["ix"]))
            iy.append(int(row["iy"]))
            x.append(float(row["x_um"]))
            y.append(float(row["y_um"]))
            i_soft.append(float(row["i_soft_A"]))
            i_hard.append(float(row["i_hard_A"]))
    x = np.array(x)
    y = np.array(y)
    s = np.array(i_soft)
    h = np.array(i_hard)
    return np.array(ix), np.array(iy), x, y, s, h, s + h


def load_mesh_voltage(tsv: Path):
    ix, iy, x, y, v = [], [], [], [], []
    with tsv.open(newline="") as f:
        r = csv.DictReader(f, delimiter="\t")
        for row in r:
            ix.append(int(row["ix"]))
            iy.append(int(row["iy"]))
            x.append(float(row["x_um"]))
            y.append(float(row["y_um"]))
            v.append(float(row["v_V"]))
    return np.array(ix), np.array(iy), np.array(x), np.array(y), np.array(v)


def scatter_field(ax, x, y, c, title, cmap, vmin, vmax, pt):
    sc = ax.scatter(
        x,
        y,
        c=c,
        s=pt,
        cmap=cmap,
        vmin=vmin,
        vmax=vmax,
        edgecolors="k",
        linewidths=0.12,
        zorder=3,
    )
    ax.set_aspect("equal")
    ax.set_title(title)
    ax.set_xlabel("x (µm)")
    ax.set_ylabel("y (µm)")
    ax.grid(True, linestyle=":", alpha=0.2)
    return sc


def cmd_current(args: argparse.Namespace, root: Path) -> None:
    mesh = args.mesh
    if not mesh.is_file():
        raise SystemExit(f"Mesh TSV not found: {mesh}")

    ix, iy, x, y, i_soft, i_hard, i_tot = load_mesh_current(mesh)
    px, py = load_pins(args.hard)
    core = load_core_rect(root)

    span = max(x.max() - x.min(), y.max() - y.min(), 1.0)
    pt = max(4.0, min(58.0, 4800.0 * span / max(len(x), 1)))
    cmax = float(np.nanmax(np.maximum(i_soft, np.maximum(i_hard, i_tot))))
    if cmax <= 0:
        cmax = 1e-15

    fig, axes = plt.subplots(1, 3, figsize=(16, 5.2), dpi=args.dpi, constrained_layout=True)
    for ax in axes:
        draw_mesh_edges(ax, ix, iy, x, y)

    sc0 = scatter_field(
        axes[0], x, y, i_soft, "Soft macro current (Σ Imax per tile)", "plasma", 0.0, cmax, pt
    )
    fig.colorbar(sc0, ax=axes[0], shrink=0.82, label="A")
    sc1 = scatter_field(
        axes[1], x, y, i_hard, "Hard macro current (at nearest node)", "plasma", 0.0, cmax, pt
    )
    fig.colorbar(sc1, ax=axes[1], shrink=0.82, label="A")
    sc2 = scatter_field(axes[2], x, y, i_tot, "Total mesh-node current", "plasma", 0.0, cmax, pt)
    fig.colorbar(sc2, ax=axes[2], shrink=0.82, label="A")

    if px.size > 0:
        axes[2].scatter(
            px,
            py,
            s=26,
            marker="^",
            facecolor="#00e396",
            edgecolors="0.2",
            linewidths=0.35,
            zorder=6,
            label="Hard PG pins",
        )
        axes[2].legend(loc="upper right", fontsize=8)

    add_core_patches(list(axes), core)
    fig.suptitle("Estimated current distribution on PDN mesh nodes (phys_load)", fontsize=12, y=1.02)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {args.out}")


def cmd_voltage(args: argparse.Namespace, root: Path) -> None:
    mesh = args.mesh
    if not mesh.is_file():
        raise SystemExit(f"Mesh TSV not found: {mesh}")

    ix, iy, x, y, v = load_mesh_voltage(mesh)
    px, py = load_pins(args.hard)
    core = load_core_rect(root)

    span = max(x.max() - x.min(), y.max() - y.min(), 1.0)
    pt = max(5.0, min(72.0, 5200.0 * span / max(len(x), 1)))
    vmin = float(np.nanmin(v))
    vmax = float(np.nanmax(v))

    fig, ax = plt.subplots(figsize=(11, 10), dpi=args.dpi)
    draw_mesh_edges(ax, ix, iy, x, y, alpha=0.35, lw=0.35)
    sc = scatter_field(ax, x, y, v, "", "RdYlGn", vmin, vmax, pt)
    ax.set_title("PDN mesh: DC node voltages + hard-macro PG pin locations")
    cb = fig.colorbar(sc, ax=ax, shrink=0.72, pad=0.02)
    cb.set_label("Solved node voltage (V)")

    if px.size > 0:
        ax.scatter(
            px,
            py,
            s=32,
            marker="^",
            facecolor="#c41e3a",
            edgecolors="0.15",
            linewidths=0.4,
            zorder=5,
            label=f"Hard macro PG pins (n={len(px)})",
        )

    if core is not None:
        lx, ly, ux, uy = core
        ax.add_patch(
            mpatches.Rectangle(
                (lx, ly),
                ux - lx,
                uy - ly,
                fill=False,
                linestyle="--",
                linewidth=1.0,
                edgecolor="0.35",
                zorder=2,
                label="Core (manifest)",
            )
        )
    if px.size > 0 or core is not None:
        ax.legend(loc="upper right", fontsize=8, framealpha=0.9)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {args.out}")


def main() -> None:
    root = repo_root_from_script()
    ap = argparse.ArgumentParser(description="Research plots (phys_load mesh outputs).")
    ap.add_argument(
        "command",
        nargs="?",
        default="current",
        choices=["current", "voltage"],
        help="Plot type (default: current)",
    )
    ap.add_argument("--mesh", type=Path, default=root / "research/out/ir_mesh_nodes.tsv")
    ap.add_argument("--hard", type=Path, default=root / "research/out/ir_hard_macros.tsv")
    ap.add_argument("--dpi", type=int, default=220)
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output PNG (default: ir_mesh_current.png or ir_mesh_voltage.png)",
    )
    args = ap.parse_args()
    if args.out is None:
        args.out = (
            root / "research/out/ir_mesh_voltage.png"
            if args.command == "voltage"
            else root / "research/out/ir_mesh_current.png"
        )

    if args.command == "voltage":
        cmd_voltage(args, root)
    else:
        cmd_current(args, root)


if __name__ == "__main__":
    main()
