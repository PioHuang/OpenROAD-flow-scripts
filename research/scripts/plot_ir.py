#!/usr/bin/env python3
"""Render static PNG heatmaps from phys_load IR drop CSV outputs.

Inputs (produced by research/build/phys_load, see research/src/main.cpp):
  <out_dir>/ir_mesh_nodes.csv      columns: layer ix iy x_um y_um i_soft_A i_hard_A i_total_A v_V is_vsrc
  <out_dir>/ir_hard_macros.csv     columns: instance fp_region pg_pin_name pin_x_um pin_y_um imax_A vpin_est_V
  <out_dir>/ir_soft_modules.csv    columns: cluster_id cluster_name instance_count i_observed_A
                                            i_worst_box_A i_mesh_sum_A vmin_tile_V
  <out_dir>/ir_via_connections.csv columns: node_a layer_a x_a_um y_a_um node_b layer_b x_b_um y_b_um g_siemens
  <out_dir>/ir_pdn_stripes.csv    columns: kind layer xlo_um ylo_um xhi_um yhi_um

For multi-layer meshes the CSV contains rows for every layer.  By default the
voltage/current heatmaps show the bottom layer (first unique layer value, e.g.
metal1). Use --layer to select a different layer for the heatmap.

Outputs:
  <out_dir>/ir_mesh_voltage.png   mesh voltage heatmap (V) with voltage source markers
  <out_dir>/ir_mesh_current.png   mesh current heatmap (A) with voltage source markers
  <out_dir>/ir_hard_pins.png      voltage heatmap + hard-macro PG pin scatter
  <out_dir>/ir_soft_clusters.png  voltage heatmap + RTLMP soft-cluster rectangles
  <out_dir>/ir_layer_drops.png    per-layer IR drop with voltage sources and via markers

The script does not recompute anything; it only reads the CSVs and the manifest
(for core/die bbox, design name, and RTLMP cluster boxes via rtl_fp).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import Normalize
from matplotlib.patches import Rectangle


def resolve_repo_root(manifest_path: Path, root_json: dict) -> Path:
    """Mirror the C++ resolver in research/src/load.cpp."""
    rr = root_json.get("repo_root")
    if rr:
        return Path(rr).resolve()
    parent = manifest_path.resolve().parent
    if parent.name == "research":
        return parent.parent
    return parent


def load_fp_boxes(manifest_path: Path, root_json: dict) -> dict[str, tuple[float, float, float, float]]:
    """Parse root.fp.txt referenced by manifest.rtl_fp. Returns {name: (lx, ly, ux, uy) um}."""
    rtl_fp = root_json.get("rtl_fp")
    if not rtl_fp:
        return {}
    repo = resolve_repo_root(manifest_path, root_json)
    fp_path = (repo / rtl_fp).resolve()
    if not fp_path.is_file():
        print(f"[warn] rtl_fp file not found: {fp_path}", file=sys.stderr)
        return {}
    dbu = float(root_json.get("fp_dbu_per_um", 2000.0))
    boxes: dict[str, tuple[float, float, float, float]] = {}
    with fp_path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 5:
                continue
            name = parts[0]
            try:
                x, y, w, h = (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
            except ValueError:
                continue
            boxes[name] = (x / dbu, y / dbu, (x + w) / dbu, (y + h) / dbu)
    return boxes


def mesh_grid(mesh: pd.DataFrame, col: str) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return (xs_um, ys_um, Z) with Z shaped (ny, nx) suitable for imshow origin='lower'."""
    nx = int(mesh["ix"].max()) + 1
    ny = int(mesh["iy"].max()) + 1
    xs = np.full(nx, np.nan)
    ys = np.full(ny, np.nan)
    for ix, g in mesh.groupby("ix"):
        xs[int(ix)] = float(g["x_um"].iloc[0])
    for iy, g in mesh.groupby("iy"):
        ys[int(iy)] = float(g["y_um"].iloc[0])
    Z = np.full((ny, nx), np.nan, dtype=float)
    for _, row in mesh.iterrows():
        Z[int(row["iy"]), int(row["ix"])] = float(row[col])
    return xs, ys, Z


def core_extent_um(root_json: dict) -> tuple[float, float, float, float]:
    core = root_json.get("layout", {}).get("core", [0.0, 0.0, 0.0, 0.0])
    if len(core) != 4:
        raise ValueError(f"manifest layout.core must have 4 entries, got {core}")
    return float(core[0]), float(core[1]), float(core[2]), float(core[3])


def draw_frame(ax: plt.Axes, core: tuple[float, float, float, float],
               die: tuple[float, float, float, float] | None) -> None:
    lx, ly, ux, uy = core
    ax.add_patch(Rectangle((lx, ly), ux - lx, uy - ly, fill=False,
                           edgecolor="black", linewidth=1.0, label="core"))
    if die is not None:
        dlx, dly, dux, duy = die
        ax.add_patch(Rectangle((dlx, dly), dux - dlx, duy - dly, fill=False,
                               edgecolor="gray", linewidth=0.7, linestyle=":", label="die"))
    ax.set_xlabel("x (um)")
    ax.set_ylabel("y (um)")
    ax.set_aspect("equal", adjustable="box")


def fig_title(design: str, mesh_mode_hint: str, vdd: float,
              vmin: float, vmax: float) -> str:
    drop_mv = (vdd - vmin) * 1000.0
    return (f"{design}  |  VDD={vdd:.4f} V  |  "
            f"V_min={vmin:.4f} V  V_max={vmax:.4f} V  drop_max={drop_mv:.2f} mV"
            + (f"  |  mesh={mesh_mode_hint}" if mesh_mode_hint else ""))


def overlay_vsrc(ax: plt.Axes, mesh: pd.DataFrame) -> None:
    """Plot voltage-source nodes (is_vsrc==1) as red diamond markers."""
    if "is_vsrc" not in mesh.columns:
        return
    vsrc = mesh[mesh["is_vsrc"] == 1]
    if vsrc.empty:
        return
    ax.scatter(vsrc["x_um"], vsrc["y_um"], s=28, marker="D",
               facecolor="#e03030", edgecolors="black", linewidths=0.35,
               zorder=8, label=f"V-source ({len(vsrc)})")


def overlay_vias(ax: plt.Axes, via_df: pd.DataFrame | None,
                 layer: str | None = None) -> None:
    """Plot via connection locations as small 'x' markers."""
    if via_df is None or via_df.empty:
        return
    if layer is not None:
        sub = via_df[(via_df["layer_a"] == layer) | (via_df["layer_b"] == layer)]
    else:
        sub = via_df
    if sub.empty:
        return
    ax.scatter(sub["x_a_um"], sub["y_a_um"], s=10, marker="x",
               color="#6e40aa", linewidths=0.5, zorder=7, alpha=0.6,
               label=f"Via ({len(sub)})")


def save_voltage(mesh: pd.DataFrame, core: tuple[float, float, float, float],
                 die: tuple[float, float, float, float], design: str, vdd: float,
                 out_png: Path, via_df: pd.DataFrame | None = None,
                 sel_layer: str | None = None) -> None:
    xs, ys, Z = mesh_grid(mesh, "v_V")
    vmin = float(np.nanmin(Z))
    vmax = float(np.nanmax(Z))
    fig, ax = plt.subplots(figsize=(8, 8))
    extent = (core[0], core[2], core[1], core[3])
    im = ax.imshow(Z, origin="lower", extent=extent, cmap="turbo_r",
                   norm=Normalize(vmin=vmin, vmax=vmax), interpolation="nearest",
                   aspect="auto")
    cbar = fig.colorbar(im, ax=ax, shrink=0.85)
    cbar.set_label("mesh node voltage (V)")
    draw_frame(ax, core, die)
    overlay_vsrc(ax, mesh)
    overlay_vias(ax, via_df, sel_layer)
    ax.set_title(fig_title(design, "", vdd, vmin, vmax))
    if ax.get_legend_handles_labels()[1]:
        ax.legend(loc="upper right", fontsize=7, framealpha=0.85)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f"[plot] wrote {out_png}  (V_min={vmin:.4f}  V_max={vmax:.4f})")


def save_current(mesh: pd.DataFrame, core: tuple[float, float, float, float],
                 die: tuple[float, float, float, float], design: str, vdd: float,
                 out_png: Path, via_df: pd.DataFrame | None = None,
                 sel_layer: str | None = None) -> None:
    xs, ys, Z = mesh_grid(mesh, "i_total_A")
    imin = float(np.nanmin(Z))
    imax = float(np.nanmax(Z))
    fig, ax = plt.subplots(figsize=(8, 8))
    extent = (core[0], core[2], core[1], core[3])
    im = ax.imshow(Z, origin="lower", extent=extent, cmap="magma",
                   norm=Normalize(vmin=imin, vmax=imax), interpolation="nearest",
                   aspect="auto")
    cbar = fig.colorbar(im, ax=ax, shrink=0.85)
    cbar.set_label("mesh node total current (A)")
    draw_frame(ax, core, die)
    overlay_vsrc(ax, mesh)
    overlay_vias(ax, via_df, sel_layer)
    ax.set_title(f"{design}  |  mesh current  |  I_max={imax:.4g} A")
    if ax.get_legend_handles_labels()[1]:
        ax.legend(loc="upper right", fontsize=7, framealpha=0.85)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f"[plot] wrote {out_png}  (I_max={imax:.4g} A)")


def save_hard_pins(mesh: pd.DataFrame, hard: pd.DataFrame,
                   core: tuple[float, float, float, float],
                   die: tuple[float, float, float, float], design: str, vdd: float,
                   out_png: Path) -> None:
    xs, ys, Z = mesh_grid(mesh, "v_V")
    vmin = float(np.nanmin(Z))
    vmax = float(np.nanmax(Z))
    fig, ax = plt.subplots(figsize=(8, 8))
    extent = (core[0], core[2], core[1], core[3])
    ax.imshow(Z, origin="lower", extent=extent, cmap="turbo_r",
              norm=Normalize(vmin=vmin, vmax=vmax), interpolation="nearest",
              aspect="auto", alpha=0.45)
    draw_frame(ax, core, die)
    if len(hard) > 0:
        imax_A = hard["imax_A"].to_numpy()
        sizes = 20.0 + 120.0 * (imax_A / max(imax_A.max(), 1e-30))
        sc = ax.scatter(hard["pin_x_um"], hard["pin_y_um"], c=hard["vpin_est_V"],
                        s=sizes, cmap="turbo_r",
                        norm=Normalize(vmin=vmin, vmax=vmax),
                        edgecolor="black", linewidth=0.3)
        cbar = fig.colorbar(sc, ax=ax, shrink=0.85)
        cbar.set_label("hard PG pin estimated voltage (V)")
        ax.set_title(f"{design}  |  {len(hard)} hard macro PG pins  |  "
                     f"I_pin_max={imax_A.max():.4g} A")
    else:
        ax.set_title(f"{design}  |  no hard-macro pin rows")
    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f"[plot] wrote {out_png}  (rows={len(hard)})")


def save_soft_clusters(mesh: pd.DataFrame, soft: pd.DataFrame,
                       fp_boxes: dict[str, tuple[float, float, float, float]],
                       core: tuple[float, float, float, float],
                       die: tuple[float, float, float, float], design: str, vdd: float,
                       out_png: Path) -> None:
    xs, ys, Z = mesh_grid(mesh, "v_V")
    vmin = float(np.nanmin(Z))
    vmax = float(np.nanmax(Z))
    fig, ax = plt.subplots(figsize=(8, 8))
    extent = (core[0], core[2], core[1], core[3])
    ax.imshow(Z, origin="lower", extent=extent, cmap="turbo_r",
              norm=Normalize(vmin=vmin, vmax=vmax), interpolation="nearest",
              aspect="auto", alpha=0.35)
    draw_frame(ax, core, die)

    matched = 0
    unmatched = 0
    v_tile = soft["vmin_tile_V"].to_numpy(dtype=float)
    finite = np.isfinite(v_tile)
    if finite.any():
        v_lo = float(np.nanmin(v_tile[finite]))
        v_hi = float(np.nanmax(v_tile[finite]))
    else:
        v_lo, v_hi = vmin, vmax
    v_lo = min(v_lo, vmin)
    v_hi = max(v_hi, vmax)
    norm = Normalize(vmin=v_lo, vmax=v_hi)
    cmap = plt.get_cmap("turbo_r")

    for _, row in soft.iterrows():
        name = str(row["cluster_name"])
        box = fp_boxes.get(name)
        if box is None:
            unmatched += 1
            continue
        lx, ly, ux, uy = box
        v = float(row["vmin_tile_V"]) if np.isfinite(row["vmin_tile_V"]) else np.nan
        face = cmap(norm(v)) if np.isfinite(v) else (0.5, 0.5, 0.5, 0.4)
        ax.add_patch(Rectangle((lx, ly), ux - lx, uy - ly, facecolor=face,
                               edgecolor="black", linewidth=0.4, alpha=0.7))
        matched += 1

    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])
    cbar = fig.colorbar(sm, ax=ax, shrink=0.85)
    cbar.set_label("soft-cluster worst-tile voltage V_min (V)")
    ax.set_title(f"{design}  |  soft clusters: {matched} drawn, {unmatched} without fp bbox")
    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f"[plot] wrote {out_png}  (matched={matched}  unmatched={unmatched})")


def save_layer_drops(mesh_all: pd.DataFrame, core: tuple[float, float, float, float],
                     die: tuple[float, float, float, float], design: str, vdd: float,
                     via_df: pd.DataFrame | None, out_png: Path) -> None:
    """One subplot per layer showing IR drop (mV) with voltage source and via markers."""
    if "layer" not in mesh_all.columns:
        return
    layer_names = sorted([l for l in mesh_all["layer"].unique() if pd.notna(l)])
    n = len(layer_names)
    if n == 0:
        return

    fig, axes = plt.subplots(1, n, figsize=(7 * n, 7), squeeze=False)
    axes = axes[0]

    global_drop_min = float("inf")
    global_drop_max = float("-inf")
    layer_meshes: list[pd.DataFrame] = []
    for lname in layer_names:
        sub = mesh_all[mesh_all["layer"] == lname].copy()
        ux = sorted(sub["x_um"].unique())
        uy = sorted(sub["y_um"].unique())
        sub["ix"] = sub["x_um"].map({v: i for i, v in enumerate(ux)})
        sub["iy"] = sub["y_um"].map({v: i for i, v in enumerate(uy)})
        sub["drop_mV"] = (vdd - sub["v_V"]) * 1000.0
        layer_meshes.append(sub)
        dmin = float(sub["drop_mV"].min())
        dmax = float(sub["drop_mV"].max())
        if dmin < global_drop_min:
            global_drop_min = dmin
        if dmax > global_drop_max:
            global_drop_max = dmax

    norm = Normalize(vmin=global_drop_min, vmax=global_drop_max)

    for idx, (lname, sub) in enumerate(zip(layer_names, layer_meshes)):
        ax = axes[idx]
        xs, ys, Z = mesh_grid(sub, "drop_mV")
        extent = (core[0], core[2], core[1], core[3])
        im = ax.imshow(Z, origin="lower", extent=extent, cmap="hot",
                       norm=norm, interpolation="nearest", aspect="auto")
        draw_frame(ax, core, die)
        overlay_vsrc(ax, sub)
        overlay_vias(ax, via_df, lname)
        ax.set_title(f"{lname}  |  drop: {float(sub['drop_mV'].min()):.2f}"
                     f"-{float(sub['drop_mV'].max()):.2f} mV")
        if ax.get_legend_handles_labels()[1]:
            ax.legend(loc="upper right", fontsize=6, framealpha=0.85)

    fig.colorbar(plt.cm.ScalarMappable(norm=norm, cmap="hot"),
                 ax=list(axes), shrink=0.75, label="IR drop (mV)")
    fig.suptitle(f"{design}  |  Per-layer IR drop  |  VDD={vdd:.4f} V", fontsize=13)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f"[plot] wrote {out_png}  ({n} layers: {layer_names})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default="research/mempool.json", type=Path,
                    help="phys_load manifest (default: research/mempool.json)")
    ap.add_argument("--out-dir", default=None, type=Path,
                    help="directory of ir_*.csv (default: <manifest_dir>/out)")
    ap.add_argument("--vdd", type=float, default=None,
                    help="supply voltage (V) for the title; default: max v_V across mesh ring nodes")
    ap.add_argument("--layer", type=str, default=None,
                    help="layer to show in heatmaps (default: first unique layer in CSV)")
    args = ap.parse_args()

    manifest = args.manifest
    if not manifest.is_file():
        print(f"manifest not found: {manifest}", file=sys.stderr)
        return 1
    with manifest.open() as f:
        mf = json.load(f)

    out_dir = args.out_dir if args.out_dir is not None else (manifest.resolve().parent / "out")
    out_dir = out_dir.resolve()
    if not out_dir.is_dir():
        print(f"out dir not found: {out_dir}", file=sys.stderr)
        return 1

    mesh_csv = out_dir / "ir_mesh_nodes.csv"
    hard_csv = out_dir / "ir_hard_macros.csv"
    soft_csv = out_dir / "ir_soft_modules.csv"
    via_csv  = out_dir / "ir_via_connections.csv"
    for p in (mesh_csv, hard_csv, soft_csv):
        if not p.is_file():
            print(f"missing CSV: {p} (did you run research/build/phys_load?)", file=sys.stderr)
            return 1

    mesh_all = pd.read_csv(mesh_csv, sep="\t")
    hard = pd.read_csv(hard_csv, sep="\t")
    soft = pd.read_csv(soft_csv, sep="\t")
    via_df = pd.read_csv(via_csv, sep="\t") if via_csv.is_file() else None

    # Filter mesh to the selected layer (multi-layer support).
    if "layer" in mesh_all.columns:
        layers = mesh_all["layer"].unique()
        sel_layer = args.layer if args.layer else (layers[0] if len(layers) > 0 else None)
        if sel_layer and sel_layer in layers:
            mesh = mesh_all[mesh_all["layer"] == sel_layer].copy()
            # Re-index ix/iy within the selected layer so mesh_grid works.
            ux = sorted(mesh["x_um"].unique())
            uy = sorted(mesh["y_um"].unique())
            xmap = {v: i for i, v in enumerate(ux)}
            ymap = {v: i for i, v in enumerate(uy)}
            mesh["ix"] = mesh["x_um"].map(xmap)
            mesh["iy"] = mesh["y_um"].map(ymap)
            print(f"[plot] selected layer={sel_layer} ({len(mesh)} nodes, "
                  f"available layers: {list(layers)})")
        else:
            mesh = mesh_all
            sel_layer = None
    else:
        mesh = mesh_all
        sel_layer = None

    core = core_extent_um(mf)
    die_raw = mf.get("layout", {}).get("die")
    die = (float(die_raw[0]), float(die_raw[1]), float(die_raw[2]), float(die_raw[3])) \
        if isinstance(die_raw, list) and len(die_raw) == 4 else core
    design = str(mf.get("name", "design"))
    if sel_layer:
        design = f"{design} [{sel_layer}]"

    vdd = args.vdd
    if vdd is None:
        vdd = float(mesh["v_V"].max())

    fp_boxes = load_fp_boxes(manifest, mf)
    print(f"[plot] loaded {len(fp_boxes)} fp boxes from rtl_fp")

    save_voltage(mesh, core, die, design, vdd, out_dir / "ir_mesh_voltage.png",
                 via_df, sel_layer)
    save_current(mesh, core, die, design, vdd, out_dir / "ir_mesh_current.png",
                 via_df, sel_layer)
    save_hard_pins(mesh, hard, core, die, design, vdd, out_dir / "ir_hard_pins.png")
    save_soft_clusters(mesh, soft, fp_boxes, core, die, design, vdd,
                       out_dir / "ir_soft_clusters.png")
    save_layer_drops(mesh_all, core, die, str(mf.get("name", "design")), vdd,
                     via_df, out_dir / "ir_layer_drops.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
