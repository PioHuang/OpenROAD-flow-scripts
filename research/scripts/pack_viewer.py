#!/usr/bin/env python3
"""Pack phys_load IR CSVs + manifest bbox into self-contained HTML viewer(s).

Reads the same inputs as plot_ir.py, plus HTML template(s) at
research/viewer/. Emits standalone HTML file(s) with the data inlined as JSON
and Plotly.js pulled from a public CDN. No server is required; open the file
in a browser.

Modes (--mode):
  2d    - emit ir_viewer.html only (2D heatmap viewer)
  3d    - emit ir_viewer_3d.html only (3D multi-layer PDN viewer)
  both  - emit both (default)
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd


def resolve_repo_root(manifest_path: Path, root_json: dict) -> Path:
    rr = root_json.get("repo_root")
    if rr:
        return Path(rr).resolve()
    parent = manifest_path.resolve().parent
    if parent.name == "research":
        return parent.parent
    return parent


def load_fp_boxes(manifest_path: Path, root_json: dict) -> dict[str, tuple[float, float, float, float]]:
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
            try:
                x, y, w, h = (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
            except ValueError:
                continue
            boxes[parts[0]] = (x / dbu, y / dbu, (x + w) / dbu, (y + h) / dbu)
    return boxes


def mesh_to_arrays(mesh: pd.DataFrame) -> dict:
    """Convert a single-layer mesh DataFrame to the 2D array payload."""
    nx = int(mesh["ix"].max()) + 1
    ny = int(mesh["iy"].max()) + 1
    xs = [None] * nx
    ys = [None] * ny
    v  = [[None] * nx for _ in range(ny)]
    it = [[None] * nx for _ in range(ny)]
    ih = [[None] * nx for _ in range(ny)]
    is_ = [[None] * nx for _ in range(ny)]
    for _, row in mesh.iterrows():
        ix = int(row["ix"]); iy = int(row["iy"])
        if xs[ix] is None:
            xs[ix] = float(row["x_um"])
        if ys[iy] is None:
            ys[iy] = float(row["y_um"])
        v[iy][ix]  = float(row["v_V"])
        it[iy][ix] = float(row["i_total_A"])
        ih[iy][ix] = float(row["i_hard_A"])
        is_[iy][ix] = float(row["i_soft_A"])
    return {"nx": nx, "ny": ny, "x": xs, "y": ys,
            "v": v, "i_total": it, "i_hard": ih, "i_soft": is_}


def split_layers(mesh_df: pd.DataFrame) -> dict[str, pd.DataFrame]:
    """Split mesh_df by 'layer' column, re-indexing ix/iy per layer."""
    if "layer" not in mesh_df.columns or mesh_df["layer"].isna().all():
        return {"": mesh_df}
    out = {}
    for layer, grp in mesh_df.groupby("layer"):
        sub = grp.copy()
        ux = sorted(sub["x_um"].unique())
        uy = sorted(sub["y_um"].unique())
        xmap = {v: i for i, v in enumerate(ux)}
        ymap = {v: i for i, v in enumerate(uy)}
        sub["ix"] = sub["x_um"].map(xmap)
        sub["iy"] = sub["y_um"].map(ymap)
        out[str(layer)] = sub
    return out


def _safe(x):
    if isinstance(x, float) and (math.isnan(x) or math.isinf(x)):
        return None
    return x


def build_payload(manifest_path: Path, out_dir: Path, vdd_cli: float | None) -> dict:
    with manifest_path.open() as f:
        mf = json.load(f)
    core = mf.get("layout", {}).get("core", [0, 0, 0, 0])
    die  = mf.get("layout", {}).get("die",  core)
    design = str(mf.get("name", "design"))

    mesh_csv = out_dir / "ir_mesh_nodes.csv"
    hard_csv = out_dir / "ir_hard_macros.csv"
    soft_csv = out_dir / "ir_soft_modules.csv"
    for p in (mesh_csv, hard_csv, soft_csv):
        if not p.is_file():
            raise FileNotFoundError(f"missing CSV: {p} (run research/build/phys_load first)")

    mesh_df = pd.read_csv(mesh_csv, sep="\t")
    hard_df = pd.read_csv(hard_csv, sep="\t")
    soft_df = pd.read_csv(soft_csv, sep="\t")

    vdd = vdd_cli if vdd_cli is not None else float(mesh_df["v_V"].max())

    fp_boxes = load_fp_boxes(manifest_path, mf)
    soft_rows = []
    for _, r in soft_df.iterrows():
        name = str(r["cluster_name"])
        box = fp_boxes.get(name)
        lx, ly, ux, uy = box if box is not None else (float("nan"),) * 4
        soft_rows.append({
            "cluster_id": int(r["cluster_id"]),
            "cluster_name": name,
            "instance_count": int(r["instance_count"]),
            "i_observed_A": _safe(float(r["i_observed_A"])),
            "i_worst_box_A": _safe(float(r["i_worst_box_A"])),
            "i_mesh_sum_A": _safe(float(r["i_mesh_sum_A"])),
            "vmin_tile_V": _safe(float(r["vmin_tile_V"])),
            "lx": _safe(float(lx)), "ly": _safe(float(ly)),
            "ux": _safe(float(ux)), "uy": _safe(float(uy)),
        })

    hard_rows = []
    for _, r in hard_df.iterrows():
        hard_rows.append({
            "instance": str(r["instance"]),
            "fp_region": str(r["fp_region"]),
            "pg_pin_name": str(r["pg_pin_name"]),
            "pin_x_um": _safe(float(r["pin_x_um"])),
            "pin_y_um": _safe(float(r["pin_y_um"])),
            "imax_A": _safe(float(r["imax_A"])),
            "vpin_est_V": _safe(float(r["vpin_est_V"])),
        })

    # Multi-layer support: split mesh by layer, default to first layer for
    # the primary "mesh" key (backward compatible) and add "mesh_layers".
    layer_dfs = split_layers(mesh_df)
    layer_names = sorted(layer_dfs.keys())
    primary_layer = layer_names[0] if layer_names else ""
    primary_mesh = mesh_to_arrays(layer_dfs[primary_layer])
    mesh_layers = {}
    for name, sub in layer_dfs.items():
        mesh_layers[name] = mesh_to_arrays(sub)

    # Voltage source nodes extracted from is_vsrc column.
    vsrc_nodes = []
    if "is_vsrc" in mesh_df.columns:
        vsrc_rows = mesh_df[mesh_df["is_vsrc"] == 1]
        for _, r in vsrc_rows.iterrows():
            vsrc_nodes.append({
                "layer": str(r["layer"]) if "layer" in r.index else "",
                "x_um": _safe(float(r["x_um"])),
                "y_um": _safe(float(r["y_um"])),
                "v_V": _safe(float(r["v_V"])),
            })

    # Via connections (inter-layer edges).
    via_csv = out_dir / "ir_via_connections.csv"
    via_connections = []
    if via_csv.is_file():
        via_df = pd.read_csv(via_csv, sep="\t")
        for _, r in via_df.iterrows():
            via_connections.append({
                "layer_a": str(r["layer_a"]),
                "x_a_um": _safe(float(r["x_a_um"])),
                "y_a_um": _safe(float(r["y_a_um"])),
                "layer_b": str(r["layer_b"]),
                "x_b_um": _safe(float(r["x_b_um"])),
                "y_b_um": _safe(float(r["y_b_um"])),
                "g": _safe(float(r["g_siemens"])),
            })

    # PDN stripes (geometry from dump).
    stripe_csv = out_dir / "ir_pdn_stripes.csv"
    pdn_stripes = []
    if stripe_csv.is_file():
        stripe_df = pd.read_csv(stripe_csv, sep="\t")
        for _, r in stripe_df.iterrows():
            pdn_stripes.append({
                "kind": str(r["kind"]),
                "layer": str(r["layer"]),
                "xlo": _safe(float(r["xlo_um"])),
                "ylo": _safe(float(r["ylo_um"])),
                "xhi": _safe(float(r["xhi_um"])),
                "yhi": _safe(float(r["yhi_um"])),
            })

    # Z-height mapping for 3D view: evenly spaced by layer index.
    layer_z_map = {}
    z_spacing = 50.0
    for i, ln in enumerate(layer_names):
        layer_z_map[ln] = i * z_spacing

    return {
        "design": design,
        "vdd": vdd,
        "core": [float(v) for v in core],
        "die":  [float(v) for v in die],
        "mesh": primary_mesh,
        "mesh_layers": mesh_layers,
        "layer_names": layer_names,
        "hard": hard_rows,
        "soft": soft_rows,
        "vsrc_nodes": vsrc_nodes,
        "via_connections": via_connections,
        "pdn_stripes": pdn_stripes,
        "layer_z_map": layer_z_map,
    }


def write_html(template_path: Path, payload: dict, out_html: Path) -> None:
    tmpl = template_path.read_text(encoding="utf-8")
    token = "/*__IR_DATA_JSON__*/ null"
    if token not in tmpl:
        raise RuntimeError(f"template missing data token '/*__IR_DATA_JSON__*/ null': {template_path}")
    # Encoded via json.dumps with allow_nan=False (we've already nulled NaN/Inf),
    # so the resulting JavaScript literal is strictly valid JSON.
    data_js = json.dumps(payload, allow_nan=False, separators=(",", ":"))
    out_html.write_text(tmpl.replace(token, data_js), encoding="utf-8")


def main() -> int:
    viewer_dir = Path(__file__).resolve().parent.parent / "viewer"
    default_tmpl_2d = viewer_dir / "ir_viewer.html.tmpl"
    default_tmpl_3d = viewer_dir / "ir_viewer_3d.html.tmpl"
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default="research/mempool.json", type=Path,
                    help="phys_load manifest (default: research/mempool.json)")
    ap.add_argument("--out-dir", default=None, type=Path,
                    help="directory of ir_*.csv (default: <manifest_dir>/out)")
    ap.add_argument("--template", default=None, type=Path,
                    help="HTML template for 2D viewer (default: research/viewer/ir_viewer.html.tmpl)")
    ap.add_argument("--template-3d", default=None, type=Path,
                    help="HTML template for 3D viewer (default: research/viewer/ir_viewer_3d.html.tmpl)")
    ap.add_argument("--out", default=None, type=Path,
                    help="output HTML for 2D (default: <out_dir>/ir_viewer.html)")
    ap.add_argument("--out-3d", default=None, type=Path,
                    help="output HTML for 3D (default: <out_dir>/ir_viewer_3d.html)")
    ap.add_argument("--vdd", type=float, default=None,
                    help="supply voltage (V); default: max v_V in mesh")
    ap.add_argument("--mode", default="both", choices=["2d", "3d", "both"],
                    help="which viewer(s) to emit (default: both)")
    args = ap.parse_args()

    manifest = args.manifest
    if not manifest.is_file():
        print(f"manifest not found: {manifest}", file=sys.stderr)
        return 1
    out_dir = args.out_dir if args.out_dir is not None else (manifest.resolve().parent / "out")
    out_dir = out_dir.resolve()
    if not out_dir.is_dir():
        print(f"out dir not found: {out_dir}", file=sys.stderr)
        return 1

    payload = build_payload(manifest, out_dir, args.vdd)
    summary = (f"mesh {payload['mesh']['nx']}x{payload['mesh']['ny']}, "
               f"hard={len(payload['hard'])}, soft={len(payload['soft'])}, "
               f"vsrc={len(payload['vsrc_nodes'])}, "
               f"vias={len(payload['via_connections'])}, "
               f"stripes={len(payload['pdn_stripes'])}, "
               f"VDD={payload['vdd']:.4f} V")

    if args.mode in ("2d", "both"):
        tmpl_2d = args.template if args.template else default_tmpl_2d
        if not tmpl_2d.is_file():
            print(f"2D template not found: {tmpl_2d}", file=sys.stderr)
            return 1
        out_html = args.out if args.out is not None else (out_dir / "ir_viewer.html")
        write_html(tmpl_2d, payload, out_html)
        print(f"[pack-2d] wrote {out_html}  ({summary})")

    if args.mode in ("3d", "both"):
        tmpl_3d = args.template_3d if args.template_3d else default_tmpl_3d
        if not tmpl_3d.is_file():
            print(f"3D template not found: {tmpl_3d}", file=sys.stderr)
            return 1
        out_html_3d = args.out_3d if args.out_3d is not None else (out_dir / "ir_viewer_3d.html")
        write_html(tmpl_3d, payload, out_html_3d)
        print(f"[pack-3d] wrote {out_html_3d}  ({summary})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
