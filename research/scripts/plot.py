#!/usr/bin/env python3
"""
Research plotting entry point (phys_load outputs, etc.).

Run from OpenROAD-flow-scripts repo root:

  python3 research/scripts/plot.py              # same as: plot.py current
  python3 research/scripts/plot.py current      # mesh current: soft / hard / total + pins
  python3 research/scripts/plot.py voltage      # solved node voltages + pins
  python3 research/scripts/plot.py sources      # PSM sources (boxes if present) + mesh, clipped to core
  python3 research/scripts/plot.py model        # multilayer PDN model topology + coupling
  python3 research/scripts/plot.py model3d      # two interactive HTMLs (std vs macro PDN grid); needs plotly
  python3 research/scripts/plot.py pdn_tcl      # literal straps from pdn_tcl (PNG + Plotly HTML); needs plotly for 3D
  python3 research/scripts/plot.py pdn_tcl --no-gui   # write research/out/pdn_tcl_plan.png + *_stack.html only
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
import webbrowser
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401

try:
    import plotly.graph_objects as go

    _HAS_PLOTLY = True
except ImportError:
    go = None  # type: ignore[misc, assignment]
    _HAS_PLOTLY = False


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parent.parent.parent


def load_manifest_layout(
    root: Path, manifest: Path
) -> tuple[
    tuple[float, float, float, float] | None,
    tuple[float, float, float, float] | None,
    str | None,
    str | None,
    bool,
]:
    """Returns (die, core, psm_vsrc_file rel, psm_vsrc_boxes_file rel or None, ir_vsrc_center_node_only)."""
    p = root / manifest if not manifest.is_absolute() else manifest
    if not p.is_file():
        return None, None, None, None, False
    try:
        with p.open() as f:
            j = json.load(f)
        lay = j.get("layout") or {}
        die = lay.get("die")
        core = lay.get("core")
        vsrc = j.get("psm_vsrc_file")
        boxes = j.get("psm_vsrc_boxes_file")
        ir1 = bool(j.get("ir_vsrc_center_node_only") is True)
        dr = (
            (float(die[0]), float(die[1]), float(die[2]), float(die[3]))
            if die and len(die) >= 4
            else None
        )
        cr = (
            (float(core[0]), float(core[1]), float(core[2]), float(core[3]))
            if core and len(core) >= 4
            else None
        )
        vs = str(vsrc) if vsrc else None
        bx = str(boxes) if boxes else None
        return dr, cr, vs, bx, ir1
    except Exception:
        return None, None, None, None, False


def load_core_rect(root: Path) -> tuple[float, float, float, float] | None:
    _, cr, _, _, _ = load_manifest_layout(root, Path("research/mempool.json"))
    return cr


def derived_psm_vsrc_boxes_path(loc_path: Path) -> Path | None:
    if loc_path.suffix.lower() != ".loc":
        return None
    stem = loc_path.stem
    pref = "psm_vsrc_"
    if not stem.startswith(pref):
        return None
    net = stem[len(pref) :]
    return loc_path.parent / f"psm_vsrc_boxes_{net}.tsv"


def derived_pdn_vias_path(loc_path: Path) -> Path | None:
    if loc_path.suffix.lower() != ".loc":
        return None
    stem = loc_path.stem
    pref = "psm_vsrc_"
    if not stem.startswith(pref):
        return None
    net = stem[len(pref) :]
    return loc_path.parent / f"pdn_vias_{net}.tsv"


_RE_ADD_PDN_STRIPE_GRID_LAYER = re.compile(
    r"add_pdn_stripe\s+-grid\s+\{([^}]*)\}\s+-layer\s+\{([^}]+)\}",
    re.MULTILINE,
)


def pdn_layer_std_macro_flags(root: Path, manifest: Path) -> dict[str, tuple[bool, bool]]:
    """
    Parse manifest tech `pdn_tcl` for add_pdn_stripe: each layer -> (uses_std_grid, uses_macro_grid).
    std = empty grid name or `grid`; macro = any other grid (e.g. CORE_macro_grid_1).
    """
    mp = root / manifest if not manifest.is_absolute() else manifest
    if not mp.is_file():
        return {}
    try:
        with mp.open() as f:
            j = json.load(f)
        rel = (j.get("tech") or {}).get("pdn_tcl")
        if not rel:
            return {}
        p = (root / str(rel)).resolve()
        if not p.is_file():
            return {}
        text = p.read_text(errors="replace")
    except Exception:
        return {}
    std: set[str] = set()
    macro: set[str] = set()
    for m in _RE_ADD_PDN_STRIPE_GRID_LAYER.finditer(text):
        g = m.group(1).strip()
        lay = m.group(2).strip()
        if not lay:
            continue
        if g in ("", "grid"):
            std.add(lay)
        else:
            macro.add(lay)
    out: dict[str, tuple[bool, bool]] = {}
    for lay in std | macro:
        out[lay] = (lay in std, lay in macro)
    return out


_RE_ADD_PDN_STRIPE_FULL = re.compile(
    r"add_pdn_stripe\s+-grid\s+\{([^}]*)\}\s+-layer\s+\{([^}]+)\}\s+-width\s+\{([^}]+)\}\s+-pitch\s+\{([^}]+)\}\s+-offset\s+\{([^}]+)\}(?:\s+-followpins)?",
    re.MULTILINE,
)
_RE_ADD_PDN_CONNECT_FULL = re.compile(
    r"add_pdn_connect\s+-grid\s+\{([^}]+)\}\s+-layers\s+\{([^}]+)\}",
    re.MULTILINE,
)


def load_manifest_pdn_tcl(root: Path, manifest: Path) -> tuple[Path, str]:
    mp = root / manifest if not manifest.is_absolute() else manifest
    if not mp.is_file():
        raise SystemExit(f"manifest not found: {mp}")
    with mp.open() as f:
        j = json.load(f)
    rel = (j.get("tech") or {}).get("pdn_tcl")
    if not rel:
        raise SystemExit("manifest tech.pdn_tcl missing")
    path = (root / str(rel)).resolve()
    if not path.is_file():
        raise SystemExit(f"pdn_tcl file not found: {path}")
    return path, path.read_text(errors="replace")


def metal_rank_from_layer_name(name: str) -> int:
    m = re.search(r"(\d+)", name)
    return int(m.group(1)) if m else -1


def stripe_direction_horizontal(layer: str) -> bool:
    """Match research/src/pdn_model.cpp metalRank parity (odd -> horizontal)."""
    r = metal_rank_from_layer_name(layer)
    return r > 0 and (r % 2 == 1)


def fold_tcl_continuations(text: str) -> str:
    lines: list[str] = []
    carry = ""
    for raw in text.splitlines():
        s = raw.rstrip()
        if s.endswith("\\"):
            carry += s[:-1].strip() + " "
        else:
            lines.append(carry + s)
            carry = ""
    if carry.strip():
        lines.append(carry.strip())
    return "\n".join(lines)


def parse_pdn_tcl_stripes_connects(text: str) -> tuple[list[dict], list[dict]]:
    """
    Parse add_pdn_stripe / add_pdn_connect from PDN Tcl text (same intent as ChipLoader::parsePdnScript).
    Each stripe row is one Tcl call — no merging across grids.
    """
    t = fold_tcl_continuations(text)
    stripes: list[dict] = []
    for m in _RE_ADD_PDN_STRIPE_FULL.finditer(t):
        g, layer, w_s, p_s, o_s = m.group(1).strip(), m.group(2).strip(), m.group(3), m.group(4), m.group(5)
        followpins = "-followpins" in m.group(0)
        try:
            width = float(w_s)
            pitch = float(p_s)
            offset = float(o_s)
        except ValueError:
            continue
        stripes.append(
            {
                "grid": g,
                "layer": layer,
                "width": width,
                "pitch": pitch,
                "offset": offset,
                "followpins": followpins,
            }
        )
    connects: list[dict] = []
    for m in _RE_ADD_PDN_CONNECT_FULL.finditer(t):
        g = m.group(1).strip()
        parts = m.group(2).split()
        if len(parts) < 2:
            continue
        connects.append({"grid": g, "lower": parts[0].strip(), "upper": parts[1].strip()})
    return stripes, connects


def stripe_rectangles_xy_um(
    stripe: dict,
    extent: tuple[float, float, float, float],
    core: tuple[float, float, float, float] | None,
) -> list[tuple[float, float, float, float]]:
    """
    Axis-aligned rectangles (llx,lly,urx,ury) µm for one add_pdn_stripe.
    Repetition uses `extent` (typically manifest die) for offset/pitch math; results are clipped to `core` when set.
    """
    elx, ely, eux, euy = extent
    w = float(stripe["width"])
    pitch = float(stripe["pitch"])
    off = float(stripe["offset"])
    if w <= 0 or pitch <= 0:
        return []
    half = 0.5 * w
    horiz = stripe_direction_horizontal(stripe["layer"])
    out: list[tuple[float, float, float, float]] = []
    if horiz:
        centers = _stripe_centers_1d(off, pitch, half, ely, euy)
        for yc in centers:
            raw = (elx, yc - half, eux, yc + half)
            c = intersect_rect_core(raw, core) if core is not None else raw
            if c is not None and c[0] < c[2] and c[1] < c[3]:
                out.append(c)
    else:
        centers = _stripe_centers_1d(off, pitch, half, elx, eux)
        for xc in centers:
            raw = (xc - half, ely, xc + half, euy)
            c = intersect_rect_core(raw, core) if core is not None else raw
            if c is not None and c[0] < c[2] and c[1] < c[3]:
                out.append(c)
    return out


def _stripe_centers_1d(off: float, pitch: float, half_w: float, lo: float, hi: float) -> list[float]:
    if pitch <= 0:
        return []
    out: list[float] = []
    k_min = int(math.floor((lo - off + half_w) / pitch)) - 4
    k_max = int(math.ceil((hi - off - half_w) / pitch)) + 4
    for k in range(k_min, k_max + 1):
        c = off + k * pitch
        if c - half_w >= lo - 1e-9 and c + half_w <= hi + 1e-9:
            out.append(c)
    return out


def _append_wire_box_prism(
    traces: list,
    lx: float,
    ly: float,
    ux: float,
    uy: float,
    z0: float,
    z1: float,
    color: str,
    name: str,
    *,
    showlegend: bool,
) -> None:
    """Open 3D wireframe for one strap repetition (Plotly Scatter3d line segments)."""
    xs: list[float | None] = []
    ys: list[float | None] = []
    zs: list[float | None] = []

    def seg(ax: float, ay: float, az: float, bx: float, by: float, bz: float) -> None:
        xs.extend([ax, bx, None])
        ys.extend([ay, by, None])
        zs.extend([az, bz, None])

    seg(lx, ly, z0, ux, ly, z0)
    seg(ux, ly, z0, ux, uy, z0)
    seg(ux, uy, z0, lx, uy, z0)
    seg(lx, uy, z0, lx, ly, z0)
    seg(lx, ly, z1, ux, ly, z1)
    seg(ux, ly, z1, ux, uy, z1)
    seg(ux, uy, z1, lx, uy, z1)
    seg(lx, uy, z1, lx, ly, z1)
    for px, py in ((lx, ly), (ux, ly), (ux, uy), (lx, uy)):
        seg(px, py, z0, px, py, z1)
    traces.append(
        go.Scatter3d(
            x=xs,
            y=ys,
            z=zs,
            mode="lines",
            line=dict(color=color, width=1.8),
            name=name,
            showlegend=showlegend,
            legendgroup=name,
            hovertemplate=name + "<extra></extra>",
        )
    )


def cmd_pdn_tcl(args: argparse.Namespace, root: Path) -> None:
    """
    Visualize literal PDN intent from pdn_tcl only: repeating strap rectangles + declared layer pairs.
    Vias have no (x,y) in Tcl — 3D view draws dashed stack edges between layer mid-heights at core center.
    """
    path, text = load_manifest_pdn_tcl(root, args.manifest)
    die, core, _, _, _ = load_manifest_layout(root, args.manifest)
    if core is None:
        raise SystemExit("manifest layout.core required for strap clipping")
    stripes, connects = parse_pdn_tcl_stripes_connects(text)
    extent = die if die is not None else core

    out_png = args.out
    if out_png.suffix.lower() != ".png":
        out_png = out_png.with_suffix(".png")
    out_html = out_png.with_name(out_png.stem + "_stack.html")

    # --- 2D plan: every stripe repetition, one figure (alpha overlap)
    fig, ax = plt.subplots(figsize=(12.0, 11.0))
    add_die_core_patches(ax, die, core, z_die=1, z_core=2)
    cmap = plt.cm.tab20(np.linspace(0, 1, max(20, len(stripes))))
    seen_labels: set[str] = set()
    for si, s in enumerate(stripes):
        rects = stripe_rectangles_xy_um(s, extent, core)
        if not rects:
            continue
        lab = f'{s["grid"] or "grid"} / {s["layer"]}  pitch={s["pitch"]} w={s["width"]}'
        if s.get("followpins"):
            lab += " (followpins)"
        c = cmap[si % len(cmap)]
        for r in rects:
            lx, ly, ux, uy = r
            ax.add_patch(
                mpatches.Rectangle(
                    (lx, ly),
                    ux - lx,
                    uy - ly,
                    facecolor=c,
                    edgecolor="0.15",
                    linewidth=0.25,
                    alpha=0.38 if not s.get("followpins") else 0.22,
                    hatch="//" if s.get("followpins") else None,
                    label=lab if lab not in seen_labels else "_nolegend_",
                )
            )
            seen_labels.add(lab)
    ax.set_aspect("equal", adjustable="box")
    ax.set_xlabel("x (µm)")
    ax.set_ylabel("y (µm)")
    ax.set_title(f"PDN Tcl (literal straps) — {path.name}\nadd_pdn_connect: " + "; ".join(f'{c["lower"]}-{c["upper"]} ({c["grid"]})' for c in connects))
    handles, labels = ax.get_legend_handles_labels()
    by_label = dict(zip(labels, handles))
    if by_label:
        ax.legend(by_label.values(), by_label.keys(), loc="upper left", fontsize=7, framealpha=0.9)
    fig.savefig(out_png, dpi=args.dpi, bbox_inches="tight")
    print(f"Wrote {out_png}")
    if not args.no_gui:
        plt.show(block=False)

    # --- 3D stack (Plotly): one thin slab per stripe *repetition*, dashed connect lines
    if not _HAS_PLOTLY or go is None:
        print("Plotly not installed; skip 3D stack. Install: pip install plotly", file=sys.stderr)
        if not args.no_gui and plt.get_fignums():
            plt.show()
        return

    z_scale = max((core[2] - core[0]), (core[3] - core[1]), 200.0) * 0.04
    uniq_layers = sorted({s["layer"] for s in stripes}, key=metal_rank_from_layer_name)
    z_of = {nm: float(i) * z_scale for i, nm in enumerate(uniq_layers)}
    dz_slab = max(z_scale * 0.35, 0.05)

    traces: list = []
    max_total = int(args.pdn_tcl_max_boxes)
    max_per_cmd = int(args.pdn_tcl_max_per_stripe)
    drawn = 0
    legend_keys: set[str] = set()
    nc = len(cmap)
    for si, s in enumerate(stripes):
        rects = stripe_rectangles_xy_um(s, extent, core)
        if len(rects) > max_per_cmd:
            rects = rects[:max_per_cmd]
        z0 = z_of.get(s["layer"], 0.0)
        z1 = z0 + dz_slab
        rgba = cmap[si % nc]
        c = f"rgba({int(rgba[0] * 255)},{int(rgba[1] * 255)},{int(rgba[2] * 255)},{0.92})"
        gname = f'{s["grid"] or "grid"}/{s["layer"]} pitch={s["pitch"]} w={s["width"]}'
        for r in rects:
            if drawn >= max_total:
                break
            lx, ly, ux, uy = r
            show = gname not in legend_keys
            if show:
                legend_keys.add(gname)
            _append_wire_box_prism(traces, lx, ly, ux, uy, z0, z1, c, gname, showlegend=show)
            drawn += 1
        if drawn >= max_total:
            break

    cx = 0.5 * (core[0] + core[2])
    cy = 0.5 * (core[1] + core[3])
    for c in connects:
        za = z_of.get(c["lower"])
        zb = z_of.get(c["upper"])
        if za is None or zb is None:
            continue
        traces.append(
            go.Scatter3d(
                x=[cx, cx],
                y=[cy, cy],
                z=[za + 0.5 * dz_slab, zb + 0.5 * dz_slab],
                mode="lines",
                line=dict(color="rgba(30,30,30,0.75)", width=4, dash="dash"),
                name=f'connect {c["lower"]}-{c["upper"]}',
                showlegend=True,
                hovertext=f'{c["grid"]}: {c["lower"]} — {c["upper"]} (no via xy in Tcl)',
            )
        )

    fig3 = go.Figure(data=traces)
    fig3.update_layout(
        title=dict(text=f"PDN Tcl stack (literal) — {path.name}", x=0.5, xanchor="center"),
        scene=dict(
            xaxis_title="x (µm)",
            yaxis_title="y (µm)",
            zaxis_title="z: layer rank × scale (not physical die thickness)",
            aspectmode="data",
            dragmode="orbit",
            camera=dict(projection=dict(type="perspective")),
        ),
        margin=dict(l=0, r=0, t=52, b=0),
        legend=dict(itemsizing="constant"),
    )
    out_html.parent.mkdir(parents=True, exist_ok=True)
    fig3.write_html(out_html)
    print(f"Wrote {out_html}")
    if not args.no_gui:
        webbrowser.open(out_html.as_uri())
        plt.show()


def split_model_layers_3d(
    rows_sorted: list[dict],
    flags: dict[str, tuple[bool, bool]],
) -> tuple[list[dict], list[dict]]:
    """Split ir_pdn_model layer rows into (std_grid_layers, macro_grid_layers)."""
    std_r: list[dict] = []
    macro_r: list[dict] = []
    for r in rows_sorted:
        name = r["layer_name"].strip()
        sg, mg = flags.get(name, (True, False))
        if sg:
            std_r.append(r)
        if mg:
            macro_r.append(r)
    return std_r, macro_r


def load_manifest_pdn_vias_rel(root: Path, manifest: Path) -> str | None:
    p = root / manifest if not manifest.is_absolute() else manifest
    if not p.is_file():
        return None
    try:
        with p.open() as f:
            j = json.load(f)
        vv = j.get("pdn_vias_file")
        return str(vv) if vv else None
    except Exception:
        return None


def parse_pdn_vias_tsv(path: Path) -> list[tuple[float, float, str, str]]:
    vias: list[tuple[float, float, str, str]] = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            delim = "\t" if line.count("\t") >= 8 else ","
            parts = [p.strip() for p in line.split(delim) if p.strip()]
            if len(parts) >= 10 and parts[0].lower().startswith("x_um"):
                continue
            if len(parts) < 8:
                continue
            x = float(parts[0])
            y = float(parts[1])
            lower = parts[6]
            upper = parts[7]
            vias.append((x, y, lower, upper))
    return vias


def intersect_rect_core(
    r: tuple[float, float, float, float], core: tuple[float, float, float, float]
) -> tuple[float, float, float, float] | None:
    llx, lly, urx, ury = r
    clx, cly, cux, cuy = core
    nx0 = max(llx, clx)
    ny0 = max(lly, cly)
    nx1 = min(urx, cux)
    ny1 = min(ury, cuy)
    if nx0 >= nx1 or ny0 >= ny1:
        return None
    return (nx0, ny0, nx1, ny1)


def clip_rects_to_core(
    rects: list[tuple[float, float, float, float]], core: tuple[float, float, float, float] | None
) -> list[tuple[float, float, float, float]]:
    if core is None:
        return list(rects)
    out: list[tuple[float, float, float, float]] = []
    for r in rects:
        c = intersect_rect_core(r, core)
        if c is not None:
            out.append(c)
    return out


def parse_psm_vsrc_boxes_tsv(path: Path) -> tuple[np.ndarray, np.ndarray, list[tuple[float, float, float, float]]]:
    """True BPin boxes: llx lly urx ury voltage per line (tab or comma)."""
    cx_l: list[float] = []
    cy_l: list[float] = []
    rects: list[tuple[float, float, float, float]] = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            delim = "\t" if line.count("\t") >= 4 else ","
            parts = [p.strip() for p in line.split(delim) if p.strip()]
            if len(parts) >= 5 and parts[0].lower().startswith("llx"):
                continue
            if len(parts) < 5:
                raise ValueError(f"{path}: need llx lly urx ury voltage: {line!r}")
            llx, lly, urx, ury = map(float, parts[:4])
            rects.append((llx, lly, urx, ury))
            cx_l.append(0.5 * (llx + urx))
            cy_l.append(0.5 * (lly + ury))
    if not rects:
        raise ValueError(f"{path}: no box lines")
    return np.array(cx_l), np.array(cy_l), rects


def add_die_core_patches(
    ax,
    die: tuple[float, float, float, float] | None,
    core: tuple[float, float, float, float] | None,
    *,
    z_die: int = 1,
    z_core: int = 2,
) -> None:
    if die is not None:
        lx, ly, ux, uy = die
        ax.add_patch(
            mpatches.Rectangle(
                (lx, ly),
                ux - lx,
                uy - ly,
                fill=False,
                linestyle="-",
                linewidth=1.0,
                edgecolor="0.55",
                zorder=z_die,
                label="Die (manifest)",
            )
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
                linewidth=0.9,
                edgecolor="0.35",
                zorder=z_core,
                label="Core (manifest)",
            )
        )


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


def draw_mesh_edges_oriented(ax, ix, iy, x, y, orientation="both", alpha=0.3, lw=0.3):
    pos = {(int(a), int(b)): (float(xp), float(yp)) for a, b, xp, yp in zip(ix, iy, x, y)}
    for (i, j), (xp, yp) in pos.items():
        if orientation in ("H", "both") and (i + 1, j) in pos:
            x2, y2 = pos[(i + 1, j)]
            ax.plot([xp, x2], [yp, y2], "k-", lw=lw, alpha=alpha, zorder=1)
        if orientation in ("V", "both") and (i, j + 1) in pos:
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


def load_pdn_model(tsv: Path):
    rows_layer = []
    rows_via = []
    with tsv.open(newline="") as f:
        r = csv.DictReader(f, delimiter="\t")
        for row in r:
            kind = row["kind"].strip()
            if kind == "layer":
                rows_layer.append(row)
            elif kind == "via":
                rows_via.append(row)
    if not rows_layer:
        raise SystemExit(f"PDN model TSV has no layer rows: {tsv}")
    return rows_layer, rows_via


def parse_psm_vsrc_records(path: Path) -> tuple[np.ndarray, np.ndarray, list[tuple[float, float, float, float]]]:
    """
    Parse .loc lines as research/src/pdn_ir.cpp does:
    x_um, y_um, size_um, voltage -> square rect centered at (x,y) with half-side size/2.

    Returns (cx, cy) arrays of bump centers and the list of (llx,lly,urx,ury) squares.
    """
    cx_l: list[float] = []
    cy_l: list[float] = []
    rects: list[tuple[float, float, float, float]] = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 4:
                raise ValueError(f"{path}: expected x, y, size, voltage: {line!r}")
            x, y, size_um = float(parts[0]), float(parts[1]), float(parts[2])
            h = size_um * 0.5
            cx_l.append(x)
            cy_l.append(y)
            rects.append((x - h, y - h, x + h, y + h))
    if not rects:
        raise ValueError(f"{path}: no valid source lines")
    return np.array(cx_l), np.array(cy_l), rects


def parse_psm_vsrc_rects_um(path: Path) -> list[tuple[float, float, float, float]]:
    _, _, rects = parse_psm_vsrc_records(path)
    return rects


def mark_mesh_fixed_from_psm_rects(
    x: np.ndarray,
    y: np.ndarray,
    rects: list[tuple[float, float, float, float]],
    one_node_per_rect: bool = False,
) -> np.ndarray:
    """Mirrors pdn_ir.cpp mark_mesh_nodes_fixed_from_psm_rects."""
    n = x.size
    fixed = np.zeros(n, dtype=bool)
    for r in rects:
        llx, lly, urx, ury = r
        inside = (x >= llx) & (x <= urx) & (y >= lly) & (y <= ury)
        if one_node_per_rect:
            idx = np.flatnonzero(inside)
            if idx.size > 0:
                cx = (llx + urx) * 0.5
                cy = (lly + ury) * 0.5
                d = np.hypot(x[idx] - cx, y[idx] - cy)
                fixed[int(idx[int(np.argmin(d))])] = True
            elif n > 0:
                cx = (llx + urx) * 0.5
                cy = (lly + ury) * 0.5
                d = np.hypot(x - cx, y - cy)
                fixed[int(np.argmin(d))] = True
            continue
        if np.any(inside):
            fixed |= inside
            continue
        if n == 0:
            continue
        cx = (llx + urx) * 0.5
        cy = (lly + ury) * 0.5
        d = np.hypot(x - cx, y - cy)
        fixed[int(np.argmin(d))] = True
    return fixed


def bbox_union_mesh_vsrc(
    x: np.ndarray,
    y: np.ndarray,
    rects: list[tuple[float, float, float, float]],
    die: tuple[float, float, float, float] | None,
    core: tuple[float, float, float, float] | None,
    pad_frac: float = 0.02,
) -> tuple[float, float, float, float]:
    """Shared xlim/ylim for both subplots (microns)."""
    xs: list[float] = [float(x.min()), float(x.max())]
    ys: list[float] = [float(y.min()), float(y.max())]
    for r in rects:
        xs += [r[0], r[2]]
        ys += [r[1], r[3]]
    if die is not None:
        xs += [die[0], die[2]]
        ys += [die[1], die[3]]
    if core is not None:
        xs += [core[0], core[2]]
        ys += [core[1], core[3]]
    xm0, xm1 = min(xs), max(xs)
    ym0, ym1 = min(ys), max(ys)
    dx = xm1 - xm0 if xm1 > xm0 else 1.0
    dy = ym1 - ym0 if ym1 > ym0 else 1.0
    p = pad_frac
    return xm0 - dx * p, xm1 + dx * p, ym0 - dy * p, ym1 + dy * p


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


def cmd_sources(args: argparse.Namespace, root: Path) -> None:
    mesh_path = args.mesh
    if not mesh_path.is_file():
        raise SystemExit(f"Mesh TSV not found: {mesh_path}")

    die, core, vsrc_rel, boxes_rel, ir_center = load_manifest_layout(root, args.manifest)
    vsrc_path = args.vsrc
    if vsrc_path is None:
        if not vsrc_rel:
            raise SystemExit("No --vsrc and manifest has no psm_vsrc_file.")
        vsrc_path = (root / vsrc_rel).resolve()
    else:
        vsrc_path = vsrc_path if vsrc_path.is_absolute() else (root / vsrc_path).resolve()

    if not vsrc_path.is_file():
        raise SystemExit(f"PSM vsrc file not found: {vsrc_path}")

    boxes_path: Path | None = None
    if args.boxes is not None:
        boxes_path = args.boxes if args.boxes.is_absolute() else (root / args.boxes).resolve()
    elif boxes_rel:
        boxes_path = (root / boxes_rel).resolve()
    else:
        d = derived_psm_vsrc_boxes_path(vsrc_path)
        if d is not None and d.is_file():
            boxes_path = d.resolve()

    source_label = "PSM -vsrc .loc (squares)"
    if boxes_path is not None and boxes_path.is_file():
        bump_x, bump_y, rects_raw = parse_psm_vsrc_boxes_tsv(boxes_path)
        source_label = f"true IO boxes ({boxes_path.name})"
    else:
        bump_x, bump_y, rects_raw = parse_psm_vsrc_records(vsrc_path)

    rects = clip_rects_to_core(rects_raw, core)
    if not rects and rects_raw:
        print(
            "[sources plot] all source geometry is outside layout.core after clip — check manifest core vs ORFS.",
            file=sys.stderr,
        )
        rects = rects_raw

    ix, iy, x, y, _, _, _ = load_mesh_current(mesh_path)
    fixed = mark_mesh_fixed_from_psm_rects(x, y, rects, one_node_per_rect=ir_center)
    n_fix = int(np.sum(fixed))

    if core is not None:
        lx, ly, ux, uy = core
        outside = np.sum(
            (bump_x < lx) | (bump_x > ux) | (bump_y < ly) | (bump_y > uy)
        )
        if outside > 0:
            print(
                f"[sources plot] {outside}/{len(bump_x)} bump centers are outside manifest layout.core "
                f"— mesh nodes live on core only; align mempool.json core/die with the ORFS floorplan if shapes look shifted.",
                file=sys.stderr,
            )

    span = max(float(x.max() - x.min()), float(y.max() - y.min()), 1.0)
    pt = max(4.0, min(50.0, 4500.0 * span / max(len(x), 1)))

    model_ok = False
    src_layer = None
    src_orientation = "both"
    layer_rows = []
    via_rows = []
    if args.model.is_file():
        try:
            layer_rows, via_rows = load_pdn_model(args.model)
            src_layer = int(layer_rows[0]["source_layer"])
            for lr in layer_rows:
                if int(lr["layer_idx"]) == src_layer:
                    src_orientation = lr["direction"].strip().upper()
                    break
            model_ok = True
        except Exception:
            model_ok = False

    # Force consistent 3-panel layout: physical, mapping, debug summary
    fig, axes = plt.subplots(1, 3, figsize=(20, 6.2), dpi=args.dpi, constrained_layout=True)
    titles = [
        "Physical source geometry (clipped to core)",
        "Solver mapping on source-layer node graph",
        "Debug summary",
    ]
    for ax, title in zip(axes, titles):
        ax.set_aspect("equal")
        ax.set_title(title)
        ax.set_xlabel("x (µm)")
        ax.set_ylabel("y (µm)")
        ax.grid(True, linestyle=":", alpha=0.25)

    # Left: rectangles only + die/core
    ax0 = axes[0]
    sc_bump0 = None
    if ir_center:
        sc_bump0 = ax0.scatter(
            bump_x,
            bump_y,
            s=22,
            marker="+",
            c="k",
            linewidths=1.1,
            zorder=6,
            label=f"Center-picked source, n={len(bump_x)}",
        )
    for r in rects:
        llx, lly, urx, ury = r
        ax0.add_patch(
            mpatches.Rectangle(
                (llx, lly),
                urx - llx,
                ury - lly,
                facecolor=(0.25, 0.45, 0.95, 0.2),
                edgecolor="#1e5cb8",
                linewidth=0.85,
                zorder=3,
            )
        )
    add_die_core_patches(ax0, die, core)
    h_rect = mpatches.Patch(
        facecolor=(0.25, 0.45, 0.95, 0.35),
        edgecolor="#1e5cb8",
        label=f"Source region on core (n={len(rects)})",
    )
    leg_el = [h_rect]
    if sc_bump0 is not None:
        leg_el.insert(0, sc_bump0)
    if die is not None:
        leg_el.append(mpatches.Patch(fill=False, edgecolor="0.55", label="Die (manifest)"))
    if core is not None:
        leg_el.append(
            mpatches.Patch(fill=False, linestyle="--", edgecolor="0.35", label="Core (manifest)")
        )
    ax0.legend(handles=leg_el, loc="upper right", fontsize=8, framealpha=0.92)

    # Right: source-layer graph + all nodes (light) + fixed (bold) + rects outline
    ax1 = axes[1]
    draw_mesh_edges_oriented(ax1, ix, iy, x, y, orientation=src_orientation, alpha=0.28, lw=0.35)
    if ir_center:
        ax1.scatter(
            bump_x,
            bump_y,
            s=26,
            marker="+",
            c="k",
            linewidths=1.0,
            zorder=6,
            label="Center-picked source",
        )
    ax1.scatter(
        x[~fixed],
        y[~fixed],
        s=pt * 0.35,
        c="0.75",
        edgecolors="none",
        zorder=2,
        label="Mesh nodes (other)",
    )
    ax1.scatter(
        x[fixed],
        y[fixed],
        s=pt * 1.15,
        c="#e63946",
        edgecolors="0.15",
        linewidths=0.35,
        zorder=5,
        label=f"Fixed VDD nodes (n={n_fix})",
    )
    for r in rects:
        llx, lly, urx, ury = r
        ax1.add_patch(
            mpatches.Rectangle(
                (llx, lly),
                urx - llx,
                ury - lly,
                fill=False,
                linestyle="--",
                edgecolor="#1e5cb8",
                linewidth=0.75,
                alpha=0.85,
                zorder=4,
            )
        )
    add_die_core_patches(ax1, die, core)
    ax1.legend(loc="upper right", fontsize=8, framealpha=0.92)

    # Third panel: readable debug summary (text + mini stats bars), no geometric clutter.
    ax2 = axes[2]
    ax2.set_aspect("auto")
    ax2.set_xlabel("")
    ax2.set_ylabel("")
    ax2.grid(False)
    ax2.set_xticks([])
    ax2.set_yticks([])
    for sp in ax2.spines.values():
        sp.set_visible(False)

    rect_count = len(rects)
    unique_fixed = int(n_fix)
    collisions = max(rect_count - unique_fixed, 0)
    out_count = int(outside) if core is not None else 0
    mode_txt = "center-node-only" if ir_center else "patch (all inside)"
    src_layer_txt = "n/a"
    src_dir_txt = src_orientation
    via_count = len(via_rows) if model_ok else 0
    if model_ok and src_layer is not None:
        src_layer_txt = str(src_layer)

    lines = [
        f"Source data: {source_label}",
        f"Mode: {mode_txt}",
        f"Rectangles (after core clip): {rect_count}",
        f"Fixed mesh nodes: {unique_fixed}",
        f"Rect->node collisions: {collisions}",
        f"Centers outside core: {out_count}",
        f"Source layer: {src_layer_txt} ({src_dir_txt})",
        f"PDN via links: {via_count}",
        f"Mesh pitch: inferred from ir_mesh_nodes.tsv ({len(x)} nodes)",
    ]
    y0 = 0.94
    for i, t in enumerate(lines):
        ax2.text(0.03, y0 - i * 0.08, t, transform=ax2.transAxes, fontsize=9, ha="left", va="top")

    # Simple bars to make counts immediately visible.
    bar_names = ["rects", "fixed", "collisions"]
    bar_vals = np.array([rect_count, unique_fixed, collisions], dtype=float)
    vmax_bar = max(float(bar_vals.max()), 1.0)
    x0b = 0.07
    yb = 0.18
    wb = 0.78
    hb = 0.06
    colors = ["#1e5cb8", "#e63946", "#6b7280"]
    for i, (nm, vv, cc) in enumerate(zip(bar_names, bar_vals, colors)):
        yy = yb - i * 0.085
        ax2.add_patch(mpatches.Rectangle((x0b, yy), wb, hb, transform=ax2.transAxes, fill=False, edgecolor="0.75"))
        fillw = wb * (vv / vmax_bar)
        ax2.add_patch(mpatches.Rectangle((x0b, yy), fillw, hb, transform=ax2.transAxes, color=cc, alpha=0.85))
        ax2.text(x0b + wb + 0.02, yy + hb * 0.5, f"{nm}: {int(vv)}", transform=ax2.transAxes, va="center", fontsize=8)

    x0, x1, y0, y1 = bbox_union_mesh_vsrc(x, y, rects, die, core)
    axes[0].set_xlim(x0, x1)
    axes[0].set_ylim(y0, y1)
    axes[1].set_xlim(x0, x1)
    axes[1].set_ylim(y0, y1)
    axes[2].set_xlim(0.0, 1.0)
    axes[2].set_ylim(0.0, 1.0)

    mode_title = "center-node-only" if ir_center else "patch-over-rectangle"
    fig.suptitle(
        f"PSM sources: physical geometry vs solver mapping (mode={mode_title}, source-layer-aware)",
        fontsize=10.5,
        y=1.02,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {args.out}")


def cmd_model(args: argparse.Namespace, root: Path) -> None:
    model_tsv = args.model
    if not model_tsv.is_file():
        raise SystemExit(f"PDN model TSV not found: {model_tsv}")
    layers, vias = load_pdn_model(model_tsv)

    idx = np.array([int(r["layer_idx"]) for r in layers], dtype=int)
    names = [r["layer_name"] for r in layers]
    dirs = [r["direction"] for r in layers]
    gseg = np.array([float(r["g_per_segment"]) for r in layers], dtype=float)
    avgv = np.array([float(r["avg_v"]) for r in layers], dtype=float)
    unk = np.array([int(r["unknown_nodes"]) for r in layers], dtype=int)
    src_layer = int(layers[0]["source_layer"])
    load_layer = int(layers[0]["load_layer"])
    total_nodes = int(layers[0]["total_nodes"])
    fixed_nodes = int(layers[0]["fixed_nodes"])
    unknown_total = int(layers[0]["unknown_total"])

    order = np.argsort(idx)
    idx = idx[order]
    gseg = gseg[order]
    avgv = avgv[order]
    unk = unk[order]
    names = [names[i] for i in order]
    dirs = [dirs[i] for i in order]

    y = np.arange(len(idx), dtype=float)
    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(14, 6), dpi=args.dpi, constrained_layout=True)

    x0 = np.log10(np.maximum(gseg, 1e-30))
    sc = ax0.scatter(x0, y, s=np.clip(8 + unk * 0.02, 16, 120), c=avgv, cmap="viridis", zorder=4)
    for i in range(len(y)):
        ax0.text(x0[i] + 0.02, y[i], f"{names[i]} ({dirs[i]})", va="center", fontsize=8)

    for v in vias:
        a = int(v["via_from"])
        b = int(v["via_to"])
        if a in idx and b in idx:
            ia = int(np.where(idx == a)[0][0])
            ib = int(np.where(idx == b)[0][0])
            xa = x0[ia]
            xb = x0[ib]
            ya = y[ia]
            yb = y[ib]
            ax0.plot([xa, xb], [ya, yb], "k--", lw=0.9, alpha=0.6, zorder=2)

    if src_layer in idx:
        isrc = int(np.where(idx == src_layer)[0][0])
        ax0.scatter([x0[isrc]], [y[isrc]], marker="s", s=90, facecolor="none", edgecolor="red", linewidths=1.5, zorder=5)
    if load_layer in idx:
        ild = int(np.where(idx == load_layer)[0][0])
        ax0.scatter([x0[ild]], [y[ild]], marker="D", s=80, facecolor="none", edgecolor="orange", linewidths=1.5, zorder=5)

    cb = fig.colorbar(sc, ax=ax0, shrink=0.8)
    cb.set_label("Average solved voltage per layer (V)")
    ax0.set_xlabel("log10(G per segment)")
    ax0.set_ylabel("Layer index")
    ax0.set_title("Multi-layer PDN model topology")
    ax0.grid(True, linestyle=":", alpha=0.25)

    labels = [f"{names[i]}({dirs[i]})" for i in range(len(names))]
    ax1.barh(labels, unk, color="#3b82f6", alpha=0.85)
    ax1.set_xlabel("Unknown nodes in solve")
    ax1.set_title("Solve distribution across layers")
    ax1.grid(True, axis="x", linestyle=":", alpha=0.25)

    fig.suptitle(
        f"PDN model stats: total={total_nodes}, fixed={fixed_nodes}, unknown={unknown_total}, "
        f"src_layer={src_layer}, load_layer={load_layer}",
        fontsize=10.5,
        y=1.02,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {args.out}")


def _resolve_via_path_model3d(root: Path, manifest: Path) -> Path | None:
    vias_rel = load_manifest_pdn_vias_rel(root, manifest)
    if vias_rel:
        p = (root / Path(vias_rel)).resolve()
        return p if p.is_file() else None
    _die, _core, vsrc_rel, _boxes_rel, _ir_center = load_manifest_layout(root, manifest)
    if vsrc_rel:
        loc = (root / Path(vsrc_rel)).resolve()
        d = derived_pdn_vias_path(loc)
        if d is not None:
            p = d.resolve()
            return p if p.is_file() else None
    return None


def _model3d_draw_layers_mpl(
    ax,
    subset_rows: list[dict],
    pos: dict[tuple[int, int], tuple[float, float]],
    mesh_x: np.ndarray,
    mesh_y: np.ndarray,
    z_of: dict[int, float],
    layer_by_name: dict[str, int],
    vmin: float,
    vmax: float,
) -> None:
    cmap = plt.cm.viridis
    for r in subset_rows:
        li = int(r["layer_idx"])
        lname = r["layer_name"]
        ldir = r["direction"].strip().upper()
        lv = float(r["avg_v"])
        z = z_of[li]
        color = cmap((lv - vmin) / (vmax - vmin + 1e-15))
        for (i, j), (px, py) in pos.items():
            if ldir in ("H", "BOTH") and (i + 1, j) in pos:
                x2, y2 = pos[(i + 1, j)]
                ax.plot([px, x2], [py, y2], [z, z], color="k", alpha=0.18, lw=0.3)
            if ldir in ("V", "BOTH") and (i, j + 1) in pos:
                x2, y2 = pos[(i, j + 1)]
                ax.plot([px, x2], [py, y2], [z, z], color="k", alpha=0.18, lw=0.3)
        ax.scatter(mesh_x, mesh_y, np.full_like(mesh_x, z), s=3, c=[color], alpha=0.75, depthshade=False)
        ax.text(float(np.min(mesh_x)), float(np.max(mesh_y)), z, f"L{li} {lname} ({ldir})", fontsize=8)


def _model3d_draw_vias_mpl(
    ax,
    subset_names: set[str],
    subset_idx: set[int],
    via_path: Path | None,
    vias_meta: list[dict],
    layer_by_name: dict[str, int],
    z_of: dict[int, float],
    mesh_x: np.ndarray,
    mesh_y: np.ndarray,
) -> tuple[int, bool]:
    """Returns (physical_via_count, used_abstract)."""
    via_drawn = 0
    if via_path is not None and via_path.is_file():
        for vx, vy, lower, upper in parse_pdn_vias_tsv(via_path):
            if lower not in subset_names or upper not in subset_names:
                continue
            a = layer_by_name.get(lower)
            b = layer_by_name.get(upper)
            if a is None or b is None or a not in z_of or b not in z_of:
                continue
            ax.plot([vx, vx], [vy, vy], [z_of[a], z_of[b]], color="#6d28d9", alpha=0.68, lw=2.2)
            via_drawn += 1
    if via_drawn > 0:
        return via_drawn, False
    n = len(mesh_x)
    sample_idx = np.arange(n) if n <= 220 else np.linspace(0, n - 1, 220, dtype=int)
    for v in vias_meta:
        a = int(v["via_from"])
        b = int(v["via_to"])
        if a not in subset_idx or b not in subset_idx:
            continue
        if a not in z_of or b not in z_of:
            continue
        za, zb = z_of[a], z_of[b]
        for k in sample_idx:
            ax.plot([mesh_x[k], mesh_x[k]], [mesh_y[k], mesh_y[k]], [za, zb], color="#7c3aed", alpha=0.32, lw=1.2)
    return 0, True


def _model3d_figure_plotly(
    title: str,
    subset_rows: list[dict],
    pos: dict[tuple[int, int], tuple[float, float]],
    mesh_x: np.ndarray,
    mesh_y: np.ndarray,
    z_of: dict[int, float],
    layer_by_name: dict[str, int],
    subset_names: set[str],
    subset_idx: set[int],
    via_path: Path | None,
    vias_meta: list[dict],
    src_layer: int,
    load_layer: int,
    vmin: float,
    vmax: float,
) -> "go.Figure":
    assert go is not None
    traces: list = []
    cmap = plt.cm.viridis

    for r in subset_rows:
        li = int(r["layer_idx"])
        ldir = r["direction"].strip().upper()
        lv = float(r["avg_v"])
        z = z_of[li]
        rgba = cmap((lv - vmin) / (vmax - vmin + 1e-15))
        c = f"rgba({int(rgba[0]*255)},{int(rgba[1]*255)},{int(rgba[2]*255)},0.85)"
        xs: list[float] = []
        ys: list[float] = []
        zs: list[float] = []
        cap = 35000
        for (i, j), (px, py) in pos.items():
            if len(xs) >= cap:
                break
            if ldir in ("H", "BOTH") and (i + 1, j) in pos:
                x2, y2 = pos[(i + 1, j)]
                xs += [px, x2, None]
                ys += [py, y2, None]
                zs += [z, z, None]
            if ldir in ("V", "BOTH") and (i, j + 1) in pos:
                x2, y2 = pos[(i, j + 1)]
                xs += [px, x2, None]
                ys += [py, y2, None]
                zs += [z, z, None]
        if xs:
            traces.append(
                go.Scatter3d(
                    x=xs,
                    y=ys,
                    z=zs,
                    mode="lines",
                    line=dict(color="rgba(0,0,0,0.25)", width=2),
                    name=f"edges L{li}",
                    showlegend=False,
                    hoverinfo="skip",
                )
            )
        traces.append(
            go.Scatter3d(
                x=mesh_x,
                y=mesh_y,
                z=np.full_like(mesh_x, z, dtype=float),
                mode="markers",
                marker=dict(size=2, color=c),
                name=f"L{li} {r['layer_name']}",
                showlegend=True,
            )
        )

    vx_l: list[float] = []
    vy_l: list[float] = []
    vz_l: list[float] = []
    if via_path is not None and via_path.is_file():
        vrows = [
            t
            for t in parse_pdn_vias_tsv(via_path)
            if t[2] in subset_names and t[3] in subset_names
        ]
        max_via = 4000
        if len(vrows) > max_via:
            rng = np.random.default_rng(0)
            pick = rng.choice(len(vrows), size=max_via, replace=False)
            vrows = [vrows[int(i)] for i in pick]
        for vx, vy, lower, upper in vrows:
            a = layer_by_name.get(lower)
            b = layer_by_name.get(upper)
            if a is None or b is None or a not in z_of or b not in z_of:
                continue
            vx_l += [vx, vx, None]
            vy_l += [vy, vy, None]
            vz_l += [z_of[a], z_of[b], None]
    if vx_l:
        traces.append(
            go.Scatter3d(
                x=vx_l,
                y=vy_l,
                z=vz_l,
                mode="lines",
                line=dict(color="rgba(109,40,217,0.75)", width=3),
                name="vias (exported sites)",
                showlegend=True,
            )
        )
    else:
        n = len(mesh_x)
        sample_idx = np.arange(n) if n <= 220 else np.linspace(0, n - 1, 220, dtype=int)
        ax_x: list[float] = []
        ax_y: list[float] = []
        ax_z: list[float] = []
        for v in vias_meta:
            a = int(v["via_from"])
            b = int(v["via_to"])
            if a not in subset_idx or b not in subset_idx or a not in z_of or b not in z_of:
                continue
            za, zb = z_of[a], z_of[b]
            for k in sample_idx:
                ax_x += [float(mesh_x[k]), float(mesh_x[k]), None]
                ax_y += [float(mesh_y[k]), float(mesh_y[k]), None]
                ax_z += [za, zb, None]
        if ax_x:
            traces.append(
                go.Scatter3d(
                    x=ax_x,
                    y=ax_y,
                    z=ax_z,
                    mode="lines",
                    line=dict(color="rgba(124,58,237,0.45)", width=2),
                    name="vias (abstract sample)",
                    showlegend=True,
                )
            )

    fig = go.Figure(data=traces)
    fig.update_layout(
        title=dict(text=title, x=0.5, xanchor="center"),
        scene=dict(
            xaxis_title="x (µm)",
            yaxis_title="y (µm)",
            zaxis_title="stacked layer plane (µm offset)",
            aspectmode="data",
            dragmode="orbit",
            camera=dict(projection=dict(type="perspective")),
        ),
        margin=dict(l=0, r=0, t=56, b=0),
        legend=dict(yanchor="top", y=0.99, xanchor="left", x=0.01),
    )
    if src_layer in z_of:
        fig.add_trace(
            go.Scatter3d(
                x=[float(np.max(mesh_x))],
                y=[float(np.max(mesh_y))],
                z=[z_of[src_layer]],
                mode="text",
                text=["SOURCE"],
                textfont=dict(color="red", size=12),
                name="source layer",
                showlegend=False,
            )
        )
    if load_layer in z_of:
        fig.add_trace(
            go.Scatter3d(
                x=[float(np.max(mesh_x))],
                y=[float(np.min(mesh_y))],
                z=[z_of[load_layer]],
                mode="text",
                text=["LOAD"],
                textfont=dict(color="orange", size=12),
                name="load layer",
                showlegend=False,
            )
        )
    return fig


def cmd_model3d(args: argparse.Namespace, root: Path) -> None:
    model_tsv = args.model
    mesh_tsv = args.mesh
    if not model_tsv.is_file():
        raise SystemExit(f"PDN model TSV not found: {model_tsv}")
    if not mesh_tsv.is_file():
        raise SystemExit(f"Mesh TSV not found: {mesh_tsv}")

    layers, vias = load_pdn_model(model_tsv)
    ix, iy, x, y, _, _, _ = load_mesh_current(mesh_tsv)

    rows_sorted = sorted(layers, key=lambda r: int(r["layer_idx"]))
    layer_indices = [int(r["layer_idx"]) for r in rows_sorted]
    layer_names = [r["layer_name"] for r in rows_sorted]
    layer_dirs = [r["direction"].strip().upper() for r in rows_sorted]
    layer_avgv = [float(r["avg_v"]) for r in rows_sorted]
    src_layer = int(rows_sorted[0]["source_layer"])
    load_layer = int(rows_sorted[0]["load_layer"])
    layer_by_name = {r["layer_name"]: int(r["layer_idx"]) for r in rows_sorted}

    if len(layer_indices) == 1:
        z_spacing = 1.0
    else:
        z_spacing = max(float(max(x) - min(x)), float(max(y) - min(y)), 1.0) * 0.18
    z_of = {li: i * z_spacing for i, li in enumerate(layer_indices)}

    pos = {(int(a), int(b)): (float(px), float(py)) for a, b, px, py in zip(ix, iy, x, y)}
    vmax = max(layer_avgv) if layer_avgv else 1.0
    vmin = min(layer_avgv) if layer_avgv else 0.0

    grid_flags = pdn_layer_std_macro_flags(root, args.manifest)
    std_rows, macro_rows = split_model_layers_3d(rows_sorted, grid_flags)
    via_path = _resolve_via_path_model3d(root, args.manifest)

    out_stem = args.out
    if out_stem.suffix.lower() in (".png", ".html"):
        out_stem = out_stem.with_suffix("")
    out_stem.parent.mkdir(parents=True, exist_ok=True)

    def emit_one(
        label: str,
        subset: list[dict],
        suffix: str,
    ) -> None:
        if not subset:
            note = f"(no {label} layers in model; check PDN Tcl add_pdn_stripe -grid)"
            if _HAS_PLOTLY and go is not None and not args.matplotlib:
                fig = go.Figure()
                fig.add_annotation(
                    text=note,
                    xref="paper",
                    yref="paper",
                    x=0.5,
                    y=0.5,
                    showarrow=False,
                    font=dict(size=14),
                )
                fig.update_layout(title=f"PDN 3D — {label}")
                outp = out_stem.parent / f"{out_stem.name}_{suffix}.html"
                fig.write_html(outp, include_plotlyjs="cdn", config={"displayModeBar": True})
                print(f"Wrote {outp}")
            else:
                fig = plt.figure(figsize=(8, 5), dpi=args.dpi)
                ax = fig.add_subplot(111)
                ax.text(0.5, 0.5, note, ha="center", va="center", transform=ax.transAxes, fontsize=11)
                ax.axis("off")
                outp = out_stem.parent / f"{out_stem.name}_{suffix}.png"
                fig.savefig(outp, dpi=args.dpi, bbox_inches="tight")
                plt.close(fig)
                print(f"Wrote {outp}")
            return

        names = {r["layer_name"].strip() for r in subset}
        idxs = {int(r["layer_idx"]) for r in subset}

        if _HAS_PLOTLY and go is not None and not args.matplotlib:
            fig = _model3d_figure_plotly(
                title=f"PDN 3D — {label} (drag to orbit)",
                subset_rows=subset,
                pos=pos,
                mesh_x=x,
                mesh_y=y,
                z_of=z_of,
                layer_by_name=layer_by_name,
                subset_names=names,
                subset_idx=idxs,
                via_path=via_path,
                vias_meta=vias,
                src_layer=src_layer,
                load_layer=load_layer,
                vmin=vmin,
                vmax=vmax,
            )
            outp = out_stem.parent / f"{out_stem.name}_{suffix}.html"
            fig.write_html(outp, include_plotlyjs="cdn", config={"displayModeBar": True})
            print(f"Wrote {outp} (interactive: orbit / zoom / pan)")
        else:
            if not _HAS_PLOTLY and not args.matplotlib:
                print(
                    "Note: install plotly for interactive 3D:  pip install plotly\n"
                    "      Falling back to matplotlib PNG (fixed view). Use --matplotlib to silence this.",
                    file=sys.stderr,
                )
            fig = plt.figure(figsize=(11, 7.5), dpi=args.dpi, constrained_layout=True)
            ax = fig.add_subplot(111, projection="3d")
            _model3d_draw_layers_mpl(ax, subset, pos, x, y, z_of, layer_by_name, vmin, vmax)
            n_phys, abstract = _model3d_draw_vias_mpl(
                ax, names, idxs, via_path, vias, layer_by_name, z_of, x, y
            )
            ax.set_title(f"PDN 3D — {label}")
            ax.set_xlabel("x (µm)")
            ax.set_ylabel("y (µm)")
            ax.set_zlabel("layer plane")
            ax.view_init(elev=22, azim=-52)
            if src_layer in z_of:
                ax.text(float(np.max(x)), float(np.max(y)), z_of[src_layer], "SOURCE", color="red", fontsize=8)
            if load_layer in z_of:
                ax.text(float(np.max(x)), float(np.min(y)), z_of[load_layer], "LOAD", color="orange", fontsize=8)
            fig.suptitle(
                f"{label}  layers={len(subset)}  via_sites={n_phys if n_phys else ('abstract' if abstract else 0)}",
                fontsize=10,
                y=0.99,
            )
            outp = out_stem.parent / f"{out_stem.name}_{suffix}.png"
            fig.savefig(outp, dpi=args.dpi, bbox_inches="tight")
            plt.close(fig)
            print(f"Wrote {outp}")

    emit_one("standard cell / core grid (`grid`)", std_rows, "std")
    emit_one("hard macro grids (non-`grid` add_pdn_stripe)", macro_rows, "macro")


def main() -> None:
    root = repo_root_from_script()
    ap = argparse.ArgumentParser(description="Research plots (phys_load mesh outputs).")
    ap.add_argument(
        "command",
        nargs="?",
        default="current",
        choices=["current", "voltage", "sources", "model", "model3d", "pdn_tcl"],
        help="Plot type (default: current)",
    )
    ap.add_argument("--mesh", type=Path, default=root / "research/out/ir_mesh_nodes.tsv")
    ap.add_argument("--hard", type=Path, default=root / "research/out/ir_hard_macros.tsv")
    ap.add_argument("--model", type=Path, default=root / "research/out/ir_pdn_model.tsv")
    ap.add_argument(
        "--manifest",
        type=Path,
        default=Path("research/mempool.json"),
        help="Manifest for die/core and default psm_vsrc_file (sources command)",
    )
    ap.add_argument(
        "--vsrc",
        type=Path,
        default=None,
        help="Override PSM vsrc .loc path (default: manifest psm_vsrc_file relative to repo root)",
    )
    ap.add_argument(
        "--boxes",
        type=Path,
        default=None,
        help="Override psm_vsrc_boxes_*.tsv (default: manifest or sibling of .loc)",
    )
    ap.add_argument("--dpi", type=int, default=220)
    ap.add_argument(
        "--matplotlib",
        action="store_true",
        help="model3d: write static PNGs instead of interactive Plotly HTML",
    )
    ap.add_argument(
        "--no-gui",
        action="store_true",
        help="pdn_tcl: write PNG/HTML only; do not open matplotlib or browser",
    )
    ap.add_argument(
        "--pdn-tcl-max-boxes",
        type=int,
        default=8000,
        help="pdn_tcl 3D: cap total strap repetition wireframes (performance)",
    )
    ap.add_argument(
        "--pdn-tcl-max-per-stripe",
        type=int,
        default=400,
        help="pdn_tcl 3D: cap repetitions per add_pdn_stripe command before global cap",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output path (PNG for most commands; model3d uses stem → stem_std.html + stem_macro.html)",
    )
    args = ap.parse_args()
    if args.out is None:
        if args.command == "voltage":
            args.out = root / "research/out/ir_mesh_voltage.png"
        elif args.command == "sources":
            args.out = root / "research/out/ir_mesh_sources.png"
        elif args.command == "model":
            args.out = root / "research/out/ir_pdn_model.png"
        elif args.command == "model3d":
            args.out = root / "research/out/ir_pdn_model_3d"
        elif args.command == "pdn_tcl":
            args.out = root / "research/out/pdn_tcl_plan.png"
        else:
            args.out = root / "research/out/ir_mesh_current.png"

    if args.command == "voltage":
        cmd_voltage(args, root)
    elif args.command == "sources":
        cmd_sources(args, root)
    elif args.command == "model":
        cmd_model(args, root)
    elif args.command == "model3d":
        cmd_model3d(args, root)
    elif args.command == "pdn_tcl":
        cmd_pdn_tcl(args, root)
    else:
        cmd_current(args, root)


if __name__ == "__main__":
    main()
