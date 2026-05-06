"""
1142PDN floorplan-stage DC PDN baseline.

This is a research abstraction: it follows the reference pre-RTL grid steady-state
stencil (anisotropic Rx/Ry, two-rail Norton pads, etc.) but is **not** bit-for-bit
equivalent to signoff tools. Thicknesses are heuristics, stripe X/Y assignment is
a convention, and package / GND modeling omits details present in full simulators.

**Grid size:** ``nx = max(2, ceil(core_width_um / mesh_pitch_um) + 1)`` (and the
same pattern for ``ny``). Nodes sit on a **uniform** rectilinear lattice from
lower-left to upper-right of ``layout.core``; ``mesh_pitch_um`` is the target edge
length — the code snaps the opposite corner so spacing is ``width/(nx-1)``.

Public entry: ``run(manifest_path, output_dir)``.

**PDN from OpenROAD ODB:** After ``pdngen``, set ``EXPORT_PDN_VIAS=1`` and/or
``EXPORT_PDN_WIRES=1`` in ORFS (see ``flow/scripts/pdn.tcl``). Point
``pdn_geometry.odb`` (read via OpenDB Python **or** converted with ORFS
``flow/scripts/odb_to_def.sh`` + ``openroad`` on PATH, then parsed from DEF),
``pdn_geometry.def`` (post-PDN DEF, optional), or ``pdn_geometry.wires_tsv`` / ``vias_tsv`` exports. Set ``model.vdd_attachment`` to ``pdn_vias`` to place Norton pad stamps
at mesh nodes inside exported via boxes (otherwise geometry is still copied to
``out/pdn_from_odb_*.tsv`` for the GUI when paths are set).

**Conductance model:** ``model.conductance_model`` is ``stripe_mesh`` (default:
analytic ``Rx``/``Ry`` from ``pdn.tcl`` stripes) or ``pdn_extracted`` (same **IR
mesh** ``nx×ny`` as the instance grid; horizontal/vertical conductances come from
overlapping PDN strap boxes on ``model.pdn_extract_mesh_layers``, defaulting to
``observation_layer``). A **stripe floor** (``model.pdn_mesh_stripe_floor_weight``,
default ``1``) ensures every mesh edge has at least the analytic stripe
conductance, so the grid stays connected and vsrc / ``cell_power`` use the same
masking as ``stripe_mesh``. Via links in DEF are not modeled as separate 3D nodes;
they are implicit in the multi-layer strap overlap if you list several layers.
If
``pdn_extracted`` is requested but the wires TSV is missing or invalid, the flow
**falls back** to ``stripe_mesh`` and prints a warning.
"""

from __future__ import annotations

import csv
import json
import math
import re
import shutil
import subprocess
from collections import defaultdict
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from scipy import sparse as sp
from scipy.sparse.linalg import spsolve


@dataclass
class AxisAlignedBox:
    llx: float
    lly: float
    urx: float
    ury: float


@dataclass
class InstancePlacement:
    """One instance: bbox/centroid + optional VDD PG pin sites (µm)."""

    is_macro: bool = False
    llx_um: float = 0.0
    lly_um: float = 0.0
    urx_um: float = 0.0
    ury_um: float = 0.0
    cx_um: float = 0.0
    cy_um: float = 0.0
    vdd_pins_um: list[tuple[float, float]] = field(default_factory=list)


@dataclass
class ClusterRegion:
    name: str
    llx_um: float
    lly_um: float
    urx_um: float
    ury_um: float


def resolve_under_root(repo_root: Path, relative_path: str) -> Path:
    return (repo_root / relative_path).resolve()


def read_utf8(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def load_layer_resistance_ohm_per_um(set_rc_tcl_text: str) -> dict[str, float]:
    """
    Parse ``set_layer_rc -resistance`` from ``setRC.tcl``.

    OpenROAD uses this as **ohms per square** (sheet resistance) for the layer.
    """
    by_layer: dict[str, float] = {}
    for raw_line in set_rc_tcl_text.splitlines():
        line = raw_line.strip()
        if not line.startswith("set_layer_rc"):
            continue
        parts = line.replace("{", " ").replace("}", " ").split()
        layer_name = None
        resistance = None
        for i, word in enumerate(parts):
            if word == "-layer" and i + 1 < len(parts):
                layer_name = parts[i + 1]
            if word == "-resistance" and i + 1 < len(parts):
                try:
                    resistance = float(parts[i + 1])
                except ValueError:
                    resistance = None
        if layer_name is not None and resistance is not None:
            by_layer[layer_name] = resistance
    return by_layer


def load_pdn_stripes(pdn_tcl_text: str) -> list[tuple[str, float, float]]:
    stripes: list[tuple[str, float, float]] = []
    for raw_line in pdn_tcl_text.splitlines():
        line = raw_line.strip()
        if not line.startswith("add_pdn_stripe"):
            continue
        parts = line.replace("{", " ").replace("}", " ").split()
        layer_name = None
        width_um = None
        pitch_um = None
        for i, word in enumerate(parts):
            if word == "-layer" and i + 1 < len(parts):
                layer_name = parts[i + 1]
            if word == "-width" and i + 1 < len(parts):
                try:
                    width_um = float(parts[i + 1])
                except ValueError:
                    width_um = None
            if word == "-pitch" and i + 1 < len(parts):
                try:
                    pitch_um = float(parts[i + 1])
                except ValueError:
                    pitch_um = None
        if (
            layer_name is not None
            and width_um is not None
            and pitch_um is not None
            and pitch_um > 0
            and width_um > 0
        ):
            stripes.append((layer_name, width_um, pitch_um))
    return stripes


def load_voltage_sources(vsrc_loc_path: Path) -> list[tuple[AxisAlignedBox, float]]:
    sources: list[tuple[AxisAlignedBox, float]] = []
    for raw_line in read_utf8(vsrc_loc_path).splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 4:
            continue
        cx, cy, size_um, voltage = (
            float(parts[0]),
            float(parts[1]),
            float(parts[2]),
            float(parts[3]),
        )
        half = 0.5 * size_um
        sources.append(
            (
                AxisAlignedBox(cx - half, cy - half, cx + half, cy + half),
                voltage,
            )
        )
    return sources


def load_instance_power(
    report_tsv_path: Path,
) -> tuple[dict[str, float], float]:
    power_by_instance: dict[str, float] = {}
    inferred_voltages: list[float] = []
    with report_tsv_path.open(newline="", encoding="utf-8", errors="replace") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for row in reader:
            name = (row.get("instance") or "").strip()
            if not name:
                continue
            total_w = float(row.get("total_W") or 0.0)
            power_by_instance[name] = total_w
            if row.get("current_A"):
                try:
                    current_a = float(row["current_A"])
                    if current_a > 0:
                        inferred_voltages.append(total_w / current_a)
                except ValueError:
                    pass
    sta_voltage = float(np.median(inferred_voltages)) if inferred_voltages else 1.1
    return power_by_instance, sta_voltage


def _is_vdd_pin_name(name: str) -> bool:
    n = name.strip().upper()
    return n == "VDD" or n.startswith("VDD")


def load_instance_geometry(
    instance_geom_tsv: Path | None,
) -> dict[str, InstancePlacement]:
    """Parse ``db_instances.tsv``: per-instance bbox, centroid, macro flag, VDD PG-pin list.

    Cluster membership is **not** inferred here; it comes from manifest ``groups`` plus ``rtl_fp``.
    """
    if instance_geom_tsv is None or not instance_geom_tsv.is_file():
        return {}
    rows_by_inst: dict[str, list[dict[str, str]]] = defaultdict(list)
    with instance_geom_tsv.open(newline="", encoding="utf-8", errors="replace") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for row in reader:
            name = (row.get("instance") or "").strip()
            if name:
                rows_by_inst[name].append(row)

    out: dict[str, InstancePlacement] = {}
    for name, rows in rows_by_inst.items():
        macro = False
        llx, lly, urx, ury = 0.0, 0.0, 0.0, 0.0
        cx, cy = 0.0, 0.0
        vdd_pins: list[tuple[float, float]] = []
        for row in rows:
            im = (row.get("is_macro") or "").strip()
            if im in ("1", "true", "TRUE", "True"):
                macro = True
            try:
                llx = float(row["llx_um"])
                lly = float(row["lly_um"])
                urx = float(row["urx_um"])
                ury = float(row["ury_um"])
            except (KeyError, ValueError):
                pass
            try:
                cx = float(row["cx_um"])
                cy = float(row["cy_um"])
            except (KeyError, ValueError):
                pass
            pin_name = (row.get("pg_pin_name") or "").strip()
            if not macro or not _is_vdd_pin_name(pin_name):
                continue
            try:
                px = float(row["pg_pin_x_um"])
                py = float(row["pg_pin_y_um"])
            except (KeyError, ValueError):
                continue
            if math.isfinite(px) and math.isfinite(py):
                vdd_pins.append((px, py))
        deduped: list[tuple[float, float]] = []
        for px, py in vdd_pins:
            if not any(
                abs(px - qx) < 1e-6 and abs(py - qy) < 1e-6 for qx, qy in deduped
            ):
                deduped.append((px, py))
        out[name] = InstancePlacement(
            is_macro=macro,
            llx_um=llx,
            lly_um=lly,
            urx_um=urx,
            ury_um=ury,
            cx_um=cx,
            cy_um=cy,
            vdd_pins_um=deduped,
        )
    return out


def load_instance_to_cluster(groups_path: Path | None) -> dict[str, str]:
    if groups_path is None or not groups_path.is_file():
        return {}
    out: dict[str, str] = {}
    with groups_path.open(encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            inst = parts[0]
            # Format: instance cluster_id cluster_name depth
            cluster_name = parts[2]
            out[inst] = cluster_name
    return out


def load_rtl_fp_regions(
    rtl_fp_path: Path | None, dbu_per_um: float = 2000.0
) -> dict[str, ClusterRegion]:
    if rtl_fp_path is None or not rtl_fp_path.is_file():
        return {}
    scale = 1.0 / max(dbu_per_um, 1e-9)
    out: dict[str, ClusterRegion] = {}
    with rtl_fp_path.open(encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 5:
                continue
            name = parts[0]
            try:
                x = float(parts[1]) * scale
                y = float(parts[2]) * scale
                w = float(parts[3]) * scale
                h = float(parts[4]) * scale
            except ValueError:
                continue
            out[name] = ClusterRegion(name=name, llx_um=x, lly_um=y, urx_um=x + w, ury_um=y + h)
    return out


def resolve_cluster_regions(
    instance_to_cluster: dict[str, str],
    placement_by_instance: dict[str, InstancePlacement],
    fp_regions: dict[str, ClusterRegion],
) -> dict[str, ClusterRegion]:
    out: dict[str, ClusterRegion] = {}
    cluster_to_instances: dict[str, list[str]] = defaultdict(list)
    for inst, c in instance_to_cluster.items():
        geom = placement_by_instance.get(inst)
        # By definition here, clusters do not include hard macros.
        if geom is not None and geom.is_macro:
            continue
        cluster_to_instances[c].append(inst)
    for cname, instances in cluster_to_instances.items():
        if cname in fp_regions:
            out[cname] = fp_regions[cname]
            continue
        llx = math.inf
        lly = math.inf
        urx = -math.inf
        ury = -math.inf
        for inst in instances:
            g = placement_by_instance.get(inst)
            if g is None:
                continue
            llx = min(llx, g.llx_um)
            lly = min(lly, g.lly_um)
            urx = max(urx, g.urx_um)
            ury = max(ury, g.ury_um)
        if math.isfinite(llx) and math.isfinite(lly) and urx >= llx and ury >= lly:
            out[cname] = ClusterRegion(cname, llx, lly, urx, ury)
    return out


def metal_index_from_layer_name(layer_name: str) -> int:
    match = re.search(r"(\d+)", layer_name)
    return int(match.group(1)) if match else 4


def default_thickness_um(layer_name: str) -> float:
    n = metal_index_from_layer_name(layer_name)
    return max(0.05, 0.42 - 0.028 * n)


def grid_resistor_x_ohm(
    core_width_m: float,
    core_height_m: float,
    nr: int,
    nc: int,
    pitch_m: float,
    width_m: float,
    thick_m: float,
    rho_ohm_m: float,
) -> float:
    metal_grid_col = max(1, int(math.floor(core_width_m / (2.0 * pitch_m))))
    metal_grid_row = max(1, int(math.floor(core_height_m / (2.0 * pitch_m))))
    denom_nc = max(1, nc - 1)
    return (
        (1.0 / denom_nc)
        * (nr / metal_grid_row)
        * (rho_ohm_m * core_width_m / (width_m * thick_m))
    )


def grid_resistor_y_ohm(
    core_width_m: float,
    core_height_m: float,
    nr: int,
    nc: int,
    pitch_m: float,
    width_m: float,
    thick_m: float,
    rho_ohm_m: float,
) -> float:
    metal_grid_col = max(1, int(math.floor(core_width_m / (2.0 * pitch_m))))
    metal_grid_row = max(1, int(math.floor(core_height_m / (2.0 * pitch_m))))
    denom_nr = max(1, nr - 1)
    return (
        (1.0 / denom_nr)
        * (nc / metal_grid_col)
        * (rho_ohm_m * core_height_m / (width_m * thick_m))
    )


def merge_parallel_resistances(resistances_ohm: list[float]) -> float:
    conductance = 0.0
    for r in resistances_ohm:
        if r > 0:
            conductance += 1.0 / r
    if conductance <= 0:
        raise ValueError("no positive resistance to merge")
    return 1.0 / conductance


def compute_rx_ry_from_stripes(
    core: AxisAlignedBox,
    nx: int,
    ny: int,
    stripes: list[tuple[str, float, float]],
    rho_ohm_m: float,
) -> tuple[float, float]:
    core_width_m = (core.urx - core.llx) * 1e-6
    core_height_m = (core.ury - core.lly) * 1e-6
    nr, nc = ny, nx
    ordered = sorted(
        stripes,
        key=lambda row: metal_index_from_layer_name(row[0]),
    )
    rx_parts: list[float] = []
    ry_parts: list[float] = []
    for idx, (layer_name, width_um, pitch_um) in enumerate(ordered):
        pitch_m = pitch_um * 1e-6
        width_m = width_um * 1e-6
        thick_m = default_thickness_um(layer_name) * 1e-6
        x_directed = idx % 2 == 0
        if x_directed:
            rx_parts.append(
                grid_resistor_x_ohm(
                    core_width_m,
                    core_height_m,
                    nr,
                    nc,
                    pitch_m,
                    width_m,
                    thick_m,
                    rho_ohm_m,
                )
            )
        else:
            ry_parts.append(
                grid_resistor_y_ohm(
                    core_width_m,
                    core_height_m,
                    nr,
                    nc,
                    pitch_m,
                    width_m,
                    thick_m,
                    rho_ohm_m,
                )
            )
    if not rx_parts:
        raise ValueError("need at least one X-directed stripe for Rx")
    if not ry_parts:
        raise ValueError("need at least one Y-directed stripe for Ry")
    return merge_parallel_resistances(rx_parts), merge_parallel_resistances(
        ry_parts
    )


def vdd_pad_mask_from_boxes(
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
    ny: int,
    boxes: Sequence[AxisAlignedBox],
) -> np.ndarray:
    """Mesh nodes whose (x,y) lies inside any axis-aligned box (inclusive edges)."""
    num_nodes = nx * ny
    mask = np.zeros(num_nodes, dtype=bool)
    for iy in range(ny):
        for ix in range(nx):
            x_um, y_um = x_coords[ix], y_coords[iy]
            k = flat_node_index(ix, iy, nx)
            for box in boxes:
                if box.llx <= x_um <= box.urx and box.lly <= y_um <= box.ury:
                    mask[k] = True
                    break
    if not np.any(mask):
        raise ValueError("no mesh node lies inside any pad / PDN via box")
    return mask


def vdd_pad_mask(
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
    ny: int,
    voltage_sources: list[tuple[AxisAlignedBox, float]],
) -> np.ndarray:
    boxes = [box for box, _ in voltage_sources]
    return vdd_pad_mask_from_boxes(x_coords, y_coords, nx, ny, boxes)


def load_pdn_vias_tsv(path: Path) -> list[AxisAlignedBox]:
    """
    Parse ``pdn_vias_<NET>.tsv`` from ``export_pdn_vias.tcl`` (µm).
    Uses the via bounding box columns ``llx_um`` … ``ury_um``.
    """
    lines = [ln.strip() for ln in read_utf8(path).splitlines() if ln.strip()]
    if not lines:
        return []
    header = lines[0].lstrip("#").strip().split("\t")
    llx_i = header.index("llx_um") if "llx_um" in header else -1
    lly_i = header.index("lly_um") if "lly_um" in header else -1
    urx_i = header.index("urx_um") if "urx_um" in header else -1
    ury_i = header.index("ury_um") if "ury_um" in header else -1
    if min(llx_i, lly_i, urx_i, ury_i) < 0:
        raise ValueError(f"{path}: expected llx_um/lly_um/urx_um/ury_um in header")
    out: list[AxisAlignedBox] = []
    for ln in lines[1:]:
        if ln.startswith("#"):
            continue
        c = ln.split("\t")
        if len(c) <= max(llx_i, lly_i, urx_i, ury_i):
            continue
        llx = float(c[llx_i])
        lly = float(c[lly_i])
        urx = float(c[urx_i])
        ury = float(c[ury_i])
        if urx >= llx and ury >= lly:
            out.append(AxisAlignedBox(llx, lly, urx, ury))
    return out


def copy_pdn_export_to_output(src: Path, dst: Path) -> None:
    if not src.is_file():
        raise FileNotFoundError(f"PDN export not found: {src}")
    shutil.copyfile(src, dst)


@dataclass(frozen=True)
class PdnViaLink:
    """One exported PDN via: connects lower ↔ upper metal at (x_um, y_um)."""

    x_um: float
    y_um: float
    lower_layer: str
    upper_layer: str


def load_pdn_wires_export_rows(path: Path) -> list[tuple[str, AxisAlignedBox, str, str]]:
    """
    Rows from ``export_pdn_wires.tcl``: layer, bbox, shape, net (µm).
    Skips invalid / unknown layers.
    """
    lines = [ln.strip() for ln in read_utf8(path).splitlines() if ln.strip()]
    if not lines:
        return []
    header = lines[0].lstrip("#").strip().split("\t")
    llx_i = header.index("llx_um") if "llx_um" in header else -1
    lly_i = header.index("lly_um") if "lly_um" in header else -1
    urx_i = header.index("urx_um") if "urx_um" in header else -1
    ury_i = header.index("ury_um") if "ury_um" in header else -1
    layer_i = header.index("layer") if "layer" in header else -1
    shape_i = header.index("shape") if "shape" in header else -1
    net_i = header.index("net") if "net" in header else -1
    if min(llx_i, lly_i, urx_i, ury_i, layer_i, net_i) < 0:
        raise ValueError(f"{path}: expected llx/lly/urx/ury_um, layer, net")
    out: list[tuple[str, AxisAlignedBox, str, str]] = []
    for ln in lines[1:]:
        if ln.startswith("#"):
            continue
        c = ln.split("\t")
        if len(c) <= max(llx_i, lly_i, urx_i, ury_i, layer_i, net_i):
            continue
        layer = str(c[layer_i]).strip().lower()
        if not layer or layer == "?":
            continue
        llx = float(c[llx_i])
        lly = float(c[lly_i])
        urx = float(c[urx_i])
        ury = float(c[ury_i])
        if urx < llx or ury < lly:
            continue
        shape = str(c[shape_i]).strip() if shape_i >= 0 else ""
        net = str(c[net_i]).strip()
        out.append((layer, AxisAlignedBox(llx, lly, urx, ury), shape, net))
    return out


def load_pdn_via_connectivity(path: Path) -> list[PdnViaLink]:
    """Parse ``pdn_vias_*.tsv`` for inter-layer links (lower_layer → upper_layer)."""
    lines = [ln.strip() for ln in read_utf8(path).splitlines() if ln.strip()]
    if not lines:
        return []
    header = lines[0].lstrip("#").strip().split("\t")
    x_i = header.index("x_um") if "x_um" in header else -1
    y_i = header.index("y_um") if "y_um" in header else -1
    lo_i = header.index("lower_layer") if "lower_layer" in header else -1
    hi_i = header.index("upper_layer") if "upper_layer" in header else -1
    if min(x_i, y_i, lo_i, hi_i) < 0:
        return []
    out: list[PdnViaLink] = []
    for ln in lines[1:]:
        if ln.startswith("#"):
            continue
        c = ln.split("\t")
        if len(c) <= max(x_i, y_i, lo_i, hi_i):
            continue
        lo = str(c[lo_i]).strip().lower()
        hi = str(c[hi_i]).strip().lower()
        if not lo or not hi or lo == "?" or hi == "?":
            continue
        if lo == hi:
            continue
        x_um = float(c[x_i])
        y_um = float(c[y_i])
        if not (math.isfinite(x_um) and math.isfinite(y_um)):
            continue
        out.append(PdnViaLink(x_um, y_um, lo, hi))
    return out


def try_import_odb():
    """
    OpenROAD's ``odb`` extension (same data as ``read_db`` in the app).
    Requires a local OpenROAD build on ``PYTHONPATH`` (see OpenROAD docs).
    """
    try:
        import odb  # type: ignore[import-not-found,import-untyped]
    except ImportError:
        return None
    return odb


def _db_coord_to_um(block, dbu: int) -> float:
    dbpm = int(block.getDb().getDbuPerMicron())
    return float(dbu) / float(max(dbpm, 1))


def _sbox_via_layer_names(sbox) -> tuple[str, str]:
    """Match ``export_pdn_vias.tcl`` (tech via vs block via)."""
    lower, upper = "?", "?"
    tv = sbox.getTechVia()
    if tv is not None:
        bl = tv.getBottomLayer()
        tl = tv.getTopLayer()
        if bl is not None:
            lower = bl.getName()
        if tl is not None:
            upper = tl.getName()
    else:
        bv = sbox.getBlockVia()
        if bv is not None:
            bl = bv.getBottomLayer()
            tl = bv.getTopLayer()
            if bl is not None:
                lower = bl.getName()
            if tl is not None:
                upper = tl.getName()
    return lower.strip().lower(), upper.strip().lower()


def extract_pdn_geometry_from_odb(
    odb_path: Path,
    power_net_names: set[str] | None,
) -> tuple[list[tuple[str, AxisAlignedBox, str, str]], list[PdnViaLink]]:
    """
    Walk special POWER nets in a saved ``.odb`` (e.g. post-``pdngen``), same
    geometry ``export_pdn_wires.tcl`` / ``export_pdn_vias.tcl`` would dump.
    """
    odb = try_import_odb()
    if odb is None:
        raise ImportError(
            "Python module 'odb' not found. Add OpenROAD's build to PYTHONPATH "
            "(see OpenROAD build docs), or use pdn_geometry.wires_tsv."
        )
    _create = getattr(odb, "dbDatabase_create", None)
    if callable(_create):
        db = _create()
    else:
        try:
            from openroad import Design  # type: ignore[import-not-found]

            db = Design.createDetachedDb()
        except ImportError as exc:
            raise ImportError(
                "OpenDB Python module has no dbDatabase_create(); install the "
                "full OpenROAD Python bindings (openroad + odb) or use "
                "pdn_geometry.wires_tsv."
            ) from exc
    odb.read_db(db, str(odb_path))
    chip = db.getChip()
    if chip is None:
        raise ValueError(f"{odb_path}: database has no chip")
    block = chip.getBlock()
    if block is None:
        raise ValueError(f"{odb_path}: chip has no block")

    if power_net_names is None:
        power_net_names = {"VDD"}

    wire_out: list[tuple[str, AxisAlignedBox, str, str]] = []
    via_out: list[PdnViaLink] = []

    for net in block.getNets():
        if str(net.getSigType()) != "POWER":
            continue
        nname = net.getName()
        if nname not in power_net_names:
            continue
        for swire in net.getSWires():
            for sbox in swire.getWires():
                llx, lly, urx, ury = (
                    sbox.xMin(),
                    sbox.yMin(),
                    sbox.xMax(),
                    sbox.yMax(),
                )
                if sbox.isVia():
                    lo, hi = _sbox_via_layer_names(sbox)
                    if lo == "?" or hi == "?" or lo == hi:
                        continue
                    cx = (llx + urx) // 2
                    cy = (lly + ury) // 2
                    via_out.append(
                        PdnViaLink(
                            _db_coord_to_um(block, cx),
                            _db_coord_to_um(block, cy),
                            lo,
                            hi,
                        )
                    )
                else:
                    tl = sbox.getTechLayer()
                    if tl is None:
                        continue
                    layer = tl.getName().strip().lower()
                    if not layer:
                        continue
                    shape = str(sbox.getWireShapeType())
                    wire_out.append(
                        (
                            layer,
                            AxisAlignedBox(
                                _db_coord_to_um(block, llx),
                                _db_coord_to_um(block, lly),
                                _db_coord_to_um(block, urx),
                                _db_coord_to_um(block, ury),
                            ),
                            shape,
                            nname,
                        )
                    )
    return wire_out, via_out


def run_odb_to_def_shell(
    orfs_root: Path, odb_path: Path, def_out: Path
) -> None:
    """
    ORFS helper: ``flow/scripts/odb_to_def.sh`` + ``openroad`` (``write_def.tcl``).
    Requires ``openroad`` on ``PATH`` (same as a normal ORFS environment).
    """
    script = orfs_root / "flow" / "scripts" / "odb_to_def.sh"
    if not script.is_file():
        raise FileNotFoundError(f"odb_to_def.sh not found: {script}")
    def_out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["bash", str(script), str(odb_path), str(def_out)],
        check=True,
    )


def _def_parse_dbu_per_micron(text: str) -> int:
    m = re.search(r"UNITS\s+DISTANCE\s+MICRONS\s+(\d+)\s*;", text, re.IGNORECASE)
    return int(m.group(1)) if m else 2000


def _def_parse_via_lower_upper(text: str) -> dict[str, tuple[str, str]]:
    """From DEF VIAS section: ``+ LAYERS metal1 viaCut metal2`` (own line or same as ``- name``)."""
    out: dict[str, tuple[str, str]] = {}

    def add_layers(via_name: str, fragment: str) -> None:
        m = re.search(r"\+\s*LAYERS\s+(\S+)\s+\S+\s+(\S+)", fragment, re.IGNORECASE)
        if m:
            out[via_name] = (m.group(1).strip().lower(), m.group(2).strip().lower())

    in_vias = False
    current: str | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not in_vias:
            if re.match(r"^VIAS\s+\d+\s*;", line):
                in_vias = True
            continue
        if line.startswith("END VIAS"):
            break
        if line.startswith("- "):
            rest = line[1:].strip()
            current = rest.split()[0]
            add_layers(current, rest)
            continue
        if current and line.startswith("+ LAYERS"):
            add_layers(current, line)
    return out


def _def_resolve_star(
    tok: str, ref: int | None, fallback: int = 0
) -> int:
    if tok == "*":
        return ref if ref is not None else fallback
    return int(tok)


def _def_segment_bbox_um(
    x1: int,
    y1: int,
    x2: int,
    y2: int,
    w_dbu: int,
    dbu_per_um: int,
) -> AxisAlignedBox:
    w_um = max(float(w_dbu) / float(dbu_per_um), 1e-6)
    xf1 = float(x1) / dbu_per_um
    yf1 = float(y1) / dbu_per_um
    xf2 = float(x2) / dbu_per_um
    yf2 = float(y2) / dbu_per_um
    if abs(x2 - x1) >= abs(y2 - y1):
        yc = 0.5 * (yf1 + yf2)
        return AxisAlignedBox(
            min(xf1, xf2),
            yc - 0.5 * w_um,
            max(xf1, xf2),
            yc + 0.5 * w_um,
        )
    xc = 0.5 * (xf1 + xf2)
    return AxisAlignedBox(
        xc - 0.5 * w_um,
        min(yf1, yf2),
        xc + 0.5 * w_um,
        max(yf1, yf2),
    )


def _def_iter_specialnet_bodies(text: str) -> list[tuple[str, str]]:
    lines = text.splitlines()
    in_sn = False
    current: str | None = None
    buf: list[str] = []
    out: list[tuple[str, str]] = []
    for raw in lines:
        s = raw.strip()
        if not in_sn:
            if s.upper().startswith("SPECIALNETS"):
                in_sn = True
            continue
        if s.upper().startswith("END SPECIALNETS"):
            break
        if s.startswith("- "):
            if current is not None:
                out.append((current, "\n".join(buf)))
            buf = []
            parts = s.split()
            current = parts[1] if len(parts) > 1 else ""
            continue
        buf.append(s)
    if current is not None:
        out.append((current, "\n".join(buf)))
    return out


_re_def_seg = re.compile(
    r"(?:NEW|\+ ROUTED)\s+(\S+)\s+(\d+)\s+\+\s+SHAPE\s+(\S+)\s+"
    r"\(\s*(\S+)\s+(\S+)\s*\)\s+\(\s*(\S+)\s+(\S+)\s*\)"
)
_re_def_via_pt = re.compile(
    r"NEW\s+(\S+)\s+0\s+\+\s+SHAPE\s+(\S+)\s+"
    r"\(\s*(\S+)\s+(\S+)\s*\)\s+(\S+)"
)


def extract_pdn_geometry_from_def(
    def_path: Path,
    power_net_names: set[str] | None,
) -> tuple[list[tuple[str, AxisAlignedBox, str, str]], list[PdnViaLink]]:
    """
    Parse DEF ``SPECIALNETS`` (5.x) written by OpenROAD from post-PDN ODB.
    Complements ``extract_pdn_geometry_from_odb`` when Python ``odb`` is unavailable.
    """
    text = read_utf8(def_path)
    dbu = _def_parse_dbu_per_micron(text)
    via_def = _def_parse_via_lower_upper(text)
    if power_net_names is None:
        power_net_names = {"VDD"}

    wire_out: list[tuple[str, AxisAlignedBox, str, str]] = []
    via_out: list[PdnViaLink] = []

    for net_name, body in _def_iter_specialnet_bodies(text):
        if net_name not in power_net_names:
            continue
        last_x: int | None = None
        last_y: int | None = None
        for line in body.splitlines():
            line = line.strip()
            if not line or line.startswith("+ USE") or line.startswith("("):
                continue
            vm = _re_def_via_pt.search(line)
            if vm:
                layer = vm.group(1).strip().lower()
                shape = vm.group(2).strip()
                ax, ay, vname = vm.group(3), vm.group(4), vm.group(5).rstrip(";")
                x = _def_resolve_star(ax, last_x, 0)
                y = _def_resolve_star(ay, last_y, 0)
                last_x, last_y = x, y
                lu = via_def.get(vname)
                if lu is None:
                    continue
                lo, hi = lu
                via_out.append(
                    PdnViaLink(float(x) / dbu, float(y) / dbu, lo, hi)
                )
                continue
            sm = _re_def_seg.search(line)
            if not sm:
                continue
            layer = sm.group(1).strip().lower()
            w_dbu = int(sm.group(2))
            shape = sm.group(3).strip()
            ax, ay, bx, by = sm.group(4), sm.group(5), sm.group(6), sm.group(7)
            x1 = _def_resolve_star(ax, last_x, 0)
            y1 = _def_resolve_star(ay, last_y, 0)
            x2 = _def_resolve_star(bx, x1, x1)
            y2 = _def_resolve_star(by, y1, y1)
            last_x, last_y = x2, y2
            if w_dbu <= 0:
                continue
            box = _def_segment_bbox_um(x1, y1, x2, y2, w_dbu, dbu)
            wire_out.append((layer, box, shape, net_name))

    return wire_out, via_out


def flat_node_index(ix: int, iy: int, nx: int) -> int:
    return iy * nx + ix


def nearest_flat_index(
    cx_um: float,
    cy_um: float,
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
    ny: int,
) -> int:
    ix = int(
        np.clip(
            np.round((cx_um - x_coords[0]) / (x_coords[1] - x_coords[0] + 1e-12)),
            0,
            nx - 1,
        )
    )
    iy = int(
        np.clip(
            np.round((cy_um - y_coords[0]) / (y_coords[1] - y_coords[0] + 1e-12)),
            0,
            ny - 1,
        )
    )
    return flat_node_index(ix, iy, nx)


def distribute_power_to_cells_watts(
    power_by_instance: dict[str, float],
    placement_by_instance: dict[str, InstancePlacement],
    instance_to_cluster: dict[str, str],
    cluster_regions: dict[str, ClusterRegion],
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
    ny: int,
) -> np.ndarray:
    """
    Map instance ``total_W`` to mesh cells.

    * **Hard macro** (``is_macro`` + VDD pins): split power **evenly** across
      listed VDD PG pins; each share goes to nearest mesh node to that pin.
    * **Soft macro cluster** (from ``groups``): aggregate cluster member power and
      spread **evenly** across all mesh nodes inside the cluster region.
    * **Standard cell**: add full power at the node nearest **instance centroid**.
    """
    num_nodes = nx * ny
    cell_power = np.zeros(num_nodes, dtype=np.float64)

    def nodes_inside_bbox(geom: InstancePlacement) -> np.ndarray:
        if not (geom.llx_um <= geom.urx_um and geom.lly_um <= geom.ury_um):
            return np.array([], dtype=np.int64)
        ix0 = int(np.searchsorted(x_coords, geom.llx_um, side="left"))
        ix1 = int(np.searchsorted(x_coords, geom.urx_um, side="right")) - 1
        iy0 = int(np.searchsorted(y_coords, geom.lly_um, side="left"))
        iy1 = int(np.searchsorted(y_coords, geom.ury_um, side="right")) - 1
        ix0 = max(0, min(ix0, nx - 1))
        ix1 = max(0, min(ix1, nx - 1))
        iy0 = max(0, min(iy0, ny - 1))
        iy1 = max(0, min(iy1, ny - 1))
        if ix0 > ix1 or iy0 > iy1:
            return np.array([], dtype=np.int64)
        nodes: list[int] = []
        for iy in range(iy0, iy1 + 1):
            row_base = iy * nx
            for ix in range(ix0, ix1 + 1):
                nodes.append(row_base + ix)
        return np.array(nodes, dtype=np.int64)

    def nodes_inside_region(region: ClusterRegion) -> np.ndarray:
        ix0 = int(np.searchsorted(x_coords, region.llx_um, side="left"))
        ix1 = int(np.searchsorted(x_coords, region.urx_um, side="right")) - 1
        iy0 = int(np.searchsorted(y_coords, region.lly_um, side="left"))
        iy1 = int(np.searchsorted(y_coords, region.ury_um, side="right")) - 1
        ix0 = max(0, min(ix0, nx - 1))
        ix1 = max(0, min(ix1, nx - 1))
        iy0 = max(0, min(iy0, ny - 1))
        iy1 = max(0, min(iy1, ny - 1))
        if ix0 > ix1 or iy0 > iy1:
            return np.array([], dtype=np.int64)
        nodes: list[int] = []
        for iy in range(iy0, iy1 + 1):
            row_base = iy * nx
            for ix in range(ix0, ix1 + 1):
                nodes.append(row_base + ix)
        return np.array(nodes, dtype=np.int64)

    clustered_power: dict[str, float] = defaultdict(float)
    clustered_instances: set[str] = set()
    for name, power_w in power_by_instance.items():
        if power_w <= 0:
            continue
        g = placement_by_instance.get(name)
        if g is None:
            continue
        # Clusters do not contain hard macros (is_macro=1), regardless of pins.
        if g.is_macro:
            continue
        cname = instance_to_cluster.get(name)
        if cname and cname in cluster_regions:
            clustered_power[cname] += power_w
            clustered_instances.add(name)

    for cname, pwr in clustered_power.items():
        if pwr <= 0:
            continue
        region_nodes = nodes_inside_region(cluster_regions[cname])
        if region_nodes.size:
            cell_power[region_nodes] += pwr / region_nodes.size

    for name, power_w in power_by_instance.items():
        if power_w <= 0:
            continue
        if name in clustered_instances:
            continue
        geom = placement_by_instance.get(name)
        if geom is None:
            continue
        if geom.is_macro and geom.vdd_pins_um:
            share = power_w / len(geom.vdd_pins_um)
            for px, py in geom.vdd_pins_um:
                k = nearest_flat_index(px, py, x_coords, y_coords, nx, ny)
                cell_power[k] += share
        else:
            k = nearest_flat_index(
                geom.cx_um, geom.cy_um, x_coords, y_coords, nx, ny
            )
            cell_power[k] += power_w
    return cell_power


def apply_uniform_power_remainder(
    cell_power: np.ndarray,
    total_power: float,
    pad_mask_vdd: np.ndarray,
) -> None:
    missing = total_power - float(cell_power.sum())
    if missing <= 1e-18:
        return
    free = np.flatnonzero(~pad_mask_vdd)
    if free.size:
        cell_power[free] += missing / free.size


def stamp_anisotropic_laplacian(
    nx: int,
    ny: int,
    rx_ohm: float,
    ry_ohm: float,
) -> tuple[sp.csr_matrix, list[tuple[int, int, float]]]:
    num_nodes = nx * ny
    gx = 1.0 / rx_ohm
    gy = 1.0 / ry_ohm
    rows: list[int] = []
    cols: list[int] = []
    data: list[float] = []
    edge_list: list[tuple[int, int, float]] = []

    def stamp(row: int, col: int, value: float) -> None:
        rows.append(row)
        cols.append(col)
        data.append(value)

    for iy in range(ny):
        for ix in range(nx):
            u = flat_node_index(ix, iy, nx)
            if ix + 1 < nx:
                v = flat_node_index(ix + 1, iy, nx)
                stamp(u, u, gx)
                stamp(v, v, gx)
                stamp(u, v, -gx)
                stamp(v, u, -gx)
                edge_list.append((u, v, gx))
            if iy + 1 < ny:
                v = flat_node_index(ix, iy + 1, nx)
                stamp(u, u, gy)
                stamp(v, v, gy)
                stamp(u, v, -gy)
                stamp(v, u, -gy)
                edge_list.append((u, v, gy))
    laplacian = sp.coo_matrix(
        (data, (rows, cols)), shape=(num_nodes, num_nodes)
    ).tocsr()
    return laplacian, edge_list


def build_pdn_mesh_laplacian_from_wires(
    wire_rows: list[tuple[str, AxisAlignedBox, str, str]],
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
    ny: int,
    mesh_layer_names: set[str],
    layer_rs_ohm_per_sq: dict[str, float],
    rx_stripe_ohm: float,
    ry_stripe_ohm: float,
    min_strap_width_um: float,
    min_segment_r_ohm: float,
    max_edge_conductance_s: float,
    stripe_floor_weight: float,
) -> tuple[sp.csr_matrix, list[tuple[int, int, float]]]:
    """
    **Mesh-aligned extracted PDN:** nodes are the same ``nx×ny`` IR lattice as
    ``stamp_anisotropic_laplacian``. Each mesh **edge** gets conductance from
    overlapping SPECIALNET straps (``R = Rs·L/W``) on whitelisted layers, in
    parallel across layers. Optional **stripe floor** (per-edge minimum ``G`` from
    ``rx_stripe_ohm`` / ``ry_stripe_ohm``) keeps the grid connected when geometry
    misses an edge.

    * **Horizontal mesh edge** ``(ix,iy)-(ix+1,iy)``: straps with ``dx≥dy`` whose
      vertical span contains ``y_coords[iy]`` contribute ``G = (overlap_x/hx)·W/(Rs·hx)``.
    * **Vertical mesh edge** ``(ix,iy)-(ix,iy+1)``: straps with ``dy>dx`` whose
      horizontal span contains ``x_coords[ix]`` contribute analogously.
    """
    from collections import defaultdict

    gx_floor = (1.0 / rx_stripe_ohm) * max(stripe_floor_weight, 0.0)
    gy_floor = (1.0 / ry_stripe_ohm) * max(stripe_floor_weight, 0.0)

    g_h: dict[tuple[int, int], float] = defaultdict(float)
    g_v: dict[tuple[int, int], float] = defaultdict(float)

    def rs_for(layer: str) -> float:
        rs = layer_rs_ohm_per_sq.get(layer)
        if rs is None:
            rs = layer_rs_ohm_per_sq.get(layer.lower())
        return rs if rs is not None else 0.01

    for layer, box, _shape, _net in wire_rows:
        ly = layer.strip().lower()
        if ly not in mesh_layer_names:
            continue
        rs = rs_for(ly)
        llx = min(box.llx, box.urx)
        urx = max(box.llx, box.urx)
        lly = min(box.lly, box.ury)
        ury = max(box.lly, box.ury)
        dx = urx - llx
        dy = ury - lly
        if dx < 1e-12 and dy < 1e-12:
            continue

        if dx >= dy:
            W_m = max(min_strap_width_um, dy)
            y_lo, y_hi = lly, ury
            for iy in range(ny):
                y = float(y_coords[iy])
                if y < y_lo - 1e-9 or y > y_hi + 1e-9:
                    continue
                for ix in range(nx - 1):
                    xa = float(x_coords[ix])
                    xb = float(x_coords[ix + 1])
                    if xb < llx - 1e-9 or xa > urx + 1e-9:
                        continue
                    overlap = min(xb, urx) - max(xa, llx)
                    if overlap <= 1e-15:
                        continue
                    hx = xb - xa
                    r_seg = max(rs * hx / W_m, min_segment_r_ohm)
                    g_add = min(1.0 / r_seg, max_edge_conductance_s) * (overlap / hx)
                    g_h[(ix, iy)] += g_add
        else:
            W_m = max(min_strap_width_um, dx)
            x_lo, x_hi = llx, urx
            for ix in range(nx):
                x = float(x_coords[ix])
                if x < x_lo - 1e-9 or x > x_hi + 1e-9:
                    continue
                for iy in range(ny - 1):
                    ya = float(y_coords[iy])
                    yb = float(y_coords[iy + 1])
                    if yb < lly - 1e-9 or ya > ury + 1e-9:
                        continue
                    overlap = min(yb, ury) - max(ya, lly)
                    if overlap <= 1e-15:
                        continue
                    hy = yb - ya
                    r_seg = max(rs * hy / W_m, min_segment_r_ohm)
                    g_add = min(1.0 / r_seg, max_edge_conductance_s) * (overlap / hy)
                    g_v[(ix, iy)] += g_add

    rows: list[int] = []
    cols: list[int] = []
    data: list[float] = []
    edge_list: list[tuple[int, int, float]] = []

    def stamp_cond(u: int, v: int, g: float) -> None:
        if g <= 0:
            return
        rows.extend((u, u, v, v))
        cols.extend((v, u, u, v))
        data.extend((-g, g, -g, g))
        edge_list.append((u, v, g))

    for iy in range(ny):
        for ix in range(nx - 1):
            g_e = float(g_h[(ix, iy)])
            if stripe_floor_weight > 0:
                g_e = max(g_e, gx_floor)
            u = flat_node_index(ix, iy, nx)
            v = flat_node_index(ix + 1, iy, nx)
            stamp_cond(u, v, g_e)

    for iy in range(ny - 1):
        for ix in range(nx):
            g_e = float(g_v[(ix, iy)])
            if stripe_floor_weight > 0:
                g_e = max(g_e, gy_floor)
            u = flat_node_index(ix, iy, nx)
            v = flat_node_index(ix, iy + 1, nx)
            stamp_cond(u, v, g_e)

    num_nodes = nx * ny
    laplacian = sp.coo_matrix(
        (data, (rows, cols)), shape=(num_nodes, num_nodes), dtype=np.float64
    ).tocsr()
    return laplacian, edge_list


def build_core_grid(
    core: AxisAlignedBox, mesh_pitch_um: float
) -> tuple[int, int, np.ndarray, np.ndarray]:
    """
    Uniform ``nx * ny`` lattice covering ``core``.

    ``nx = max(2, ceil(width / mesh_pitch) + 1)`` so the shortest graph edge is
    at most about ``mesh_pitch_um`` along x (same for ``ny`` on y). Actual
    spacing is ``width / (nx - 1)`` which can be **slightly** smaller than the
    target pitch when the division does not land exactly on a pitch multiple.
    """
    width = core.urx - core.llx
    height = core.ury - core.lly
    nx = max(2, int(math.ceil(width / mesh_pitch_um)) + 1)
    ny = max(2, int(math.ceil(height / mesh_pitch_um)) + 1)
    x_coords = core.llx + (width / (nx - 1)) * np.arange(nx, dtype=np.float64)
    y_coords = core.lly + (height / (ny - 1)) * np.arange(ny, dtype=np.float64)
    return nx, ny, x_coords, y_coords


def build_two_rail_matrix(
    laplacian: sp.csr_matrix,
    pad_mask_vdd: np.ndarray,
    pad_resistance_ohm: float,
    gnd_reference_conductance: float,
) -> sp.csr_matrix:
    num_nodes = laplacian.shape[0]
    big = sp.block_diag((laplacian, laplacian), format="csr")
    pad_g = 1.0 / pad_resistance_ohm
    for k in np.flatnonzero(pad_mask_vdd):
        big[k, k] += pad_g
    big[num_nodes, num_nodes] += gnd_reference_conductance
    return big


def package_voltages_heuristic(
    total_power_w: float,
    vdd_nominal: float,
    gnd_nominal: float,
    sta_voltage: float,
    package_series_r_ohm: float,
) -> tuple[float, float]:
    v_diff = max(sta_voltage, 1e-6)
    total_current = total_power_w / v_diff
    pkg_vdd = vdd_nominal - total_current * package_series_r_ohm
    pkg_gnd = gnd_nominal + total_current * package_series_r_ohm
    return pkg_vdd, pkg_gnd


def assemble_two_rail_rhs(
    num_nodes: int,
    cell_power_w: np.ndarray,
    pad_mask_vdd: np.ndarray,
    pad_resistance_ohm: float,
    pkg_vdd: float,
    pkg_gnd: float,
    v_diff: float,
) -> np.ndarray:
    _ = pkg_gnd
    rhs = np.zeros(2 * num_nodes, dtype=np.float64)
    pad_g = 1.0 / pad_resistance_ohm
    for k in range(num_nodes):
        p_cell = cell_power_w[k]
        i_load = p_cell / v_diff
        if pad_mask_vdd[k]:
            rhs[k] += pkg_vdd * pad_g
        rhs[k] -= i_load
        rhs[num_nodes + k] += i_load
    return rhs


def solve_stationary_linear(matrix: sp.csr_matrix, rhs: np.ndarray) -> np.ndarray:
    return spsolve(matrix, rhs)


def write_mesh_nodes_tsv(
    output_dir: Path,
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
    ny: int,
    voltages: np.ndarray,
    cell_power_w: np.ndarray,
    v_diff: float,
    nominal_supply_v: float,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / "ir_mesh_nodes.tsv"
    with path.open("w", encoding="utf-8") as fh:
        fh.write(
            "ix\tiy\tx_um\ty_um\ti_soft_A\ti_hard_A\ti_total_A\tv_V\tir_drop_V\n"
        )
        vd = max(v_diff, 1e-9)
        for iy in range(ny):
            for ix in range(nx):
                k = flat_node_index(ix, iy, nx)
                load_a = cell_power_w[k] / vd
                fh.write(
                    f"{ix}\t{iy}\t{x_coords[ix]:.10g}\t{y_coords[iy]:.10g}\t"
                    f"{load_a:.10g}\t0\t{load_a:.10g}\t{voltages[k]:.10g}\t"
                    f"{nominal_supply_v - voltages[k]:.10g}\n"
                )


def write_instance_ir_report(
    output_dir: Path,
    power_by_instance: dict[str, float],
    placement_by_instance: dict[str, InstancePlacement],
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
    ny: int,
    voltages: np.ndarray,
    nominal_supply_v: float,
    observation_layer: str,
) -> None:
    # Sample V at the same (x,y) sites used for injection: per VDD pin for macros, else centroid.
    path = output_dir / "VDD_instance_ir.rpt"
    with path.open("w", encoding="utf-8") as fh:
        fh.write(
            "Instance,Terminal,Layer,X location,Y location,Voltage,IR_drop_V\n"
        )
        for name, power_w in power_by_instance.items():
            if power_w <= 0:
                continue
            geom = placement_by_instance.get(name)
            if geom is None:
                continue
            if geom.is_macro and geom.vdd_pins_um:
                sample_xy = geom.vdd_pins_um
            else:
                sample_xy = [(geom.cx_um, geom.cy_um)]
            for cx, cy in sample_xy:
                k = nearest_flat_index(cx, cy, x_coords, y_coords, nx, ny)
                vk = voltages[k]
                fh.write(
                    f"{name},VDD,{observation_layer},{cx:.4f},{cy:.4f},"
                    f"{vk:.6f},{nominal_supply_v - vk:.6f}\n"
                )


def write_edges_tsv(
    output_dir: Path,
    edge_list: list[tuple[int, int, float]],
    x_coords: np.ndarray,
    y_coords: np.ndarray,
    nx: int,
) -> None:
    path = output_dir / "edges.tsv"
    with path.open("w", encoding="utf-8") as fh:
        fh.write(
            "u\tv\tix1\tiy1\tix2\tiy2\tx1_um\ty1_um\tx2_um\ty2_um\tg_S\n"
        )
        for u, v, g_edge in edge_list:
            iy1, ix1 = divmod(u, nx)
            iy2, ix2 = divmod(v, nx)
            fh.write(
                f"{u}\t{v}\t{ix1}\t{iy1}\t{ix2}\t{iy2}\t"
                f"{x_coords[ix1]:.10g}\t{y_coords[iy1]:.10g}\t"
                f"{x_coords[ix2]:.10g}\t{y_coords[iy2]:.10g}\t{g_edge:.10g}\n"
            )


def write_vsrc_tsv(
    output_dir: Path, voltage_sources: list[tuple[AxisAlignedBox, float]]
) -> None:
    path = output_dir / "vsrc.tsv"
    with path.open("w", encoding="utf-8") as fh:
        fh.write("cx_um\tcy_um\tsize_um\tllx_um\tlly_um\turx_um\tury_um\tvoltage_V\n")
        for box, voltage in voltage_sources:
            cx = 0.5 * (box.llx + box.urx)
            cy = 0.5 * (box.lly + box.ury)
            size = max(box.urx - box.llx, box.ury - box.lly)
            fh.write(
                f"{cx:.10g}\t{cy:.10g}\t{size:.10g}\t{box.llx:.10g}\t{box.lly:.10g}\t"
                f"{box.urx:.10g}\t{box.ury:.10g}\t{voltage:.10g}\n"
            )


def write_vsrc_tsv_or_pdn_boxes(
    output_dir: Path,
    voltage_sources: list[tuple[AxisAlignedBox, float]],
    pdn_via_boxes: list[AxisAlignedBox],
    nominal_supply_v: float,
) -> None:
    """GUI / reports: mirror pad geometry into ``vsrc.tsv``."""
    if voltage_sources:
        write_vsrc_tsv(output_dir, voltage_sources)
        return
    path = output_dir / "vsrc.tsv"
    with path.open("w", encoding="utf-8") as fh:
        fh.write("cx_um\tcy_um\tsize_um\tllx_um\tlly_um\turx_um\tury_um\tvoltage_V\n")
        for box in pdn_via_boxes:
            cx = 0.5 * (box.llx + box.urx)
            cy = 0.5 * (box.lly + box.ury)
            size = max(box.urx - box.llx, box.ury - box.lly)
            fh.write(
                f"{cx:.10g}\t{cy:.10g}\t{size:.10g}\t{box.llx:.10g}\t{box.lly:.10g}\t"
                f"{box.urx:.10g}\t{box.ury:.10g}\t{nominal_supply_v:.10g}\n"
            )


def write_instance_regions_tsv(
    output_dir: Path,
    placement_by_instance: dict[str, InstancePlacement],
    cluster_regions: dict[str, ClusterRegion],
) -> None:
    path = output_dir / "instance_regions.tsv"
    with path.open("w", encoding="utf-8") as fh:
        fh.write(
            "instance\tkind\tis_macro\tllx_um\tlly_um\turx_um\tury_um\tcx_um\tcy_um\n"
        )
        for name in sorted(placement_by_instance):
            g = placement_by_instance[name]
            if g.is_macro and g.vdd_pins_um:
                kind = "hard_macro"
            else:
                kind = "instance"
            fh.write(
                f"{name}\t{kind}\t{int(g.is_macro)}\t"
                f"{g.llx_um:.10g}\t{g.lly_um:.10g}\t{g.urx_um:.10g}\t{g.ury_um:.10g}\t"
                f"{g.cx_um:.10g}\t{g.cy_um:.10g}\n"
            )
        for cname in sorted(cluster_regions):
            c = cluster_regions[cname]
            fh.write(
                f"{cname}\tcluster\t0\t{c.llx_um:.10g}\t{c.lly_um:.10g}\t"
                f"{c.urx_um:.10g}\t{c.ury_um:.10g}\t{0.5*(c.llx_um+c.urx_um):.10g}\t"
                f"{0.5*(c.lly_um+c.ury_um):.10g}\n"
            )


def run(manifest_path: str | Path, output_dir: str | Path) -> None:
    manifest_file = Path(manifest_path).resolve()
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    orfs_root = Path(manifest.get("orfs_root", ".."))
    if not orfs_root.is_absolute():
        orfs_root = (manifest_file.parent / orfs_root).resolve()
    out = Path(output_dir).resolve()

    core_list = manifest["layout"]["core"]
    core_box = AxisAlignedBox(
        float(core_list[0]),
        float(core_list[1]),
        float(core_list[2]),
        float(core_list[3]),
    )

    tech = manifest["tech"]
    pdn_path = resolve_under_root(orfs_root, tech["pdn_tcl"])
    pdn_text = read_utf8(pdn_path)
    stripes = load_pdn_stripes(pdn_text)

    model = manifest.get("model") or {}
    mesh_pitch_um = float(model.get("mesh_pitch_um", 56.0))
    observation_layer = str(model.get("observation_layer", "metal1"))
    rho_ohm_m = float(model.get("metal_resistivity_ohm_m", 1.68e-8))
    pad_r_ohm = float(model.get("pad_resistance_ohm", 10e-3))
    r_pkg_ohm = float(model.get("package_series_resistance_ohm", 1e-3))
    gnd_ref_s = float(model.get("gnd_reference_conductance_S", 1e9))
    conductance_model = str(model.get("conductance_model", "stripe_mesh")).strip().lower()

    pdn_geom = manifest.get("pdn_geometry") if isinstance(manifest.get("pdn_geometry"), dict) else {}
    pdn_vias_rel = pdn_geom.get("vias_tsv") or manifest.get("pdn_vias_tsv")
    pdn_wires_rel = pdn_geom.get("wires_tsv") or manifest.get("pdn_wires_tsv")
    pdn_odb_rel = pdn_geom.get("odb") or manifest.get("pdn_odb")
    pdn_def_rel = pdn_geom.get("def") or manifest.get("pdn_def")
    vdd_attachment = str(model.get("vdd_attachment", "vsrc_loc")).strip().lower()

    nx, ny, x_coords, y_coords = build_core_grid(core_box, mesh_pitch_um)
    rx_ohm, ry_ohm = compute_rx_ry_from_stripes(
        core_box, nx, ny, stripes, rho_ohm_m
    )

    laplacian: sp.csr_matrix
    edge_list: list[tuple[int, int, float]]
    use_pdn_extracted_graph = False
    pdn_wires_path_build: Path | None = None

    if conductance_model in ("pdn_extracted", "pdn", "odb_pdn"):
        power_raw = pdn_geom.get("power_nets")
        power_net_filter: set[str] | None = None
        if isinstance(power_raw, list) and power_raw:
            power_net_filter = {str(x) for x in power_raw}

        wire_rows: list[tuple[str, AxisAlignedBox, str, str]] = []
        via_links: list[PdnViaLink] = []
        pdn_geometry_ok = False

        if pdn_odb_rel:
            odb_abs = resolve_under_root(orfs_root, str(pdn_odb_rel))
            if not odb_abs.is_file():
                print(
                    "[1142PDN] WARNING: pdn_geometry.odb not found:\n  "
                    f"{odb_abs}",
                    flush=True,
                )
            else:
                try:
                    wire_rows, via_links = extract_pdn_geometry_from_odb(
                        odb_abs, power_net_filter
                    )
                    if wire_rows or via_links:
                        pdn_geometry_ok = True
                        print(
                            "[1142PDN] PDN geometry from ODB —",
                            str(odb_abs),
                            f"({len(wire_rows)} wire boxes, {len(via_links)} via links)",
                            flush=True,
                        )
                    else:
                        print(
                            "[1142PDN] WARNING: ODB has no PDN wire/via shapes "
                            "for the requested power nets — will try DEF / "
                            "odb_to_def.sh or wires TSV.",
                            flush=True,
                        )
                except ImportError as exc:
                    print(f"[1142PDN] WARNING: {exc}", flush=True)
                except (OSError, ValueError, RuntimeError) as exc:
                    print(
                        "[1142PDN] WARNING: reading ODB failed "
                        f"({exc!s}) — will try DEF / odb_to_def.sh or wires TSV.",
                        flush=True,
                    )

        if not pdn_geometry_ok:
            def_for_parse: Path | None = None
            if pdn_def_rel:
                def_abs = resolve_under_root(orfs_root, str(pdn_def_rel))
                if def_abs.is_file():
                    def_for_parse = def_abs
                else:
                    print(
                        "[1142PDN] WARNING: pdn_geometry.def not found:\n  "
                        f"{def_abs}",
                        flush=True,
                    )
            elif pdn_odb_rel and shutil.which("openroad"):
                odb_for_def = resolve_under_root(orfs_root, str(pdn_odb_rel))
                if odb_for_def.is_file():
                    def_for_parse = out / "pdn_from_odb.def"
                    try:
                        run_odb_to_def_shell(orfs_root, odb_for_def, def_for_parse)
                        print(
                            "[1142PDN] PDN DEF from ORFS odb_to_def.sh —",
                            str(def_for_parse),
                            flush=True,
                        )
                    except FileNotFoundError as exc:
                        print(f"[1142PDN] WARNING: {exc}", flush=True)
                        def_for_parse = None
                    except subprocess.CalledProcessError as exc:
                        print(
                            "[1142PDN] WARNING: odb_to_def.sh / openroad failed "
                            f"({exc!s}).",
                            flush=True,
                        )
                        def_for_parse = None
            if def_for_parse is not None:
                try:
                    wire_rows, via_links = extract_pdn_geometry_from_def(
                        def_for_parse, power_net_filter
                    )
                    if wire_rows or via_links:
                        pdn_geometry_ok = True
                        print(
                            "[1142PDN] PDN geometry from DEF —",
                            str(def_for_parse),
                            f"({len(wire_rows)} wire boxes, {len(via_links)} via links)",
                            flush=True,
                        )
                    else:
                        print(
                            "[1142PDN] WARNING: DEF has no matching SPECIALNETS "
                            "geometry for the requested power nets.",
                            flush=True,
                        )
                except (OSError, ValueError) as exc:
                    print(
                        "[1142PDN] WARNING: parsing DEF for PDN failed "
                        f"({exc!s}).",
                        flush=True,
                    )

        if not pdn_geometry_ok and pdn_wires_rel:
            pdn_wires_path_build = resolve_under_root(orfs_root, str(pdn_wires_rel))
            if not pdn_wires_path_build.is_file():
                print(
                    "[1142PDN] WARNING: PDN wires TSV missing:\n  "
                    f"{pdn_wires_path_build}\n"
                    "  (EXPORT_PDN_WIRES=1 after pdngen), set pdn_geometry.odb "
                    "(and openroad for odb_to_def.sh), or pdn_geometry.def.",
                    flush=True,
                )
            else:
                try:
                    wire_rows = load_pdn_wires_export_rows(pdn_wires_path_build)
                    via_links = []
                    if pdn_vias_rel:
                        vp = resolve_under_root(orfs_root, str(pdn_vias_rel))
                        if vp.is_file():
                            via_links = load_pdn_via_connectivity(vp)
                    if wire_rows or via_links:
                        pdn_geometry_ok = True
                except (OSError, ValueError) as exc:
                    print(
                        f"[1142PDN] WARNING: failed to read PDN wires TSV ({exc!s}).",
                        flush=True,
                    )

        if pdn_geometry_ok:
            try:
                set_rc_path = resolve_under_root(orfs_root, tech["set_rc_tcl"])
                layer_rs = load_layer_resistance_ohm_per_um(read_utf8(set_rc_path))
                ml_raw = model.get("pdn_extract_mesh_layers")
                if isinstance(ml_raw, list) and ml_raw:
                    mesh_layers = {str(x).strip().lower() for x in ml_raw}
                else:
                    mesh_layers = {observation_layer.strip().lower()}
                floor_w = float(model.get("pdn_mesh_stripe_floor_weight", 1.0))
                min_w = float(model.get("pdn_min_strap_width_um", 0.05))
                min_r = float(model.get("pdn_min_segment_r_ohm", 1e-6))
                max_g = float(model.get("pdn_max_edge_conductance_S", 1e9))
                laplacian, edge_list = build_pdn_mesh_laplacian_from_wires(
                    wire_rows,
                    x_coords,
                    y_coords,
                    nx,
                    ny,
                    mesh_layers,
                    layer_rs,
                    rx_ohm,
                    ry_ohm,
                    min_w,
                    min_r,
                    max_g,
                    floor_w,
                )
                use_pdn_extracted_graph = True
                print(
                    "[1142PDN] Conductance: PDN on IR mesh —",
                    nx * ny,
                    "nodes,",
                    len(edge_list),
                    "edges; layers",
                    sorted(mesh_layers),
                    f"stripe_floor_w={floor_w}",
                    flush=True,
                )
                if via_links:
                    print(
                        "[1142PDN] Note:",
                        len(via_links),
                        "via link(s) in geometry export are not separate 3D nodes "
                        "(mesh uses strap overlap on listed layers).",
                        flush=True,
                    )
            except (OSError, ValueError) as exc:
                print(
                    "[1142PDN] WARNING: PDN conductance build failed "
                    f"({exc!s}) — using stripe_mesh.",
                    flush=True,
                )
        else:
            print(
                "[1142PDN] WARNING: pdn_extracted needs pdn_geometry.odb (OpenDB "
                "or odb_to_def.sh + openroad), pdn_geometry.def, and/or "
                "pdn_geometry.wires_tsv — using stripe_mesh.",
                flush=True,
            )

    if not use_pdn_extracted_graph:
        laplacian, edge_list = stamp_anisotropic_laplacian(nx, ny, rx_ohm, ry_ohm)

    vsrc_key = manifest.get("vsrc_loc")
    voltage_sources: list[tuple[AxisAlignedBox, float]] = []
    if vsrc_key:
        vsrc_path = resolve_under_root(orfs_root, str(vsrc_key))
        voltage_sources = load_voltage_sources(vsrc_path)

    pdn_via_boxes: list[AxisAlignedBox] = []
    pdn_vias_path: Path | None = None
    if pdn_vias_rel:
        pdn_vias_path = resolve_under_root(orfs_root, str(pdn_vias_rel))
        pdn_via_boxes = load_pdn_vias_tsv(pdn_vias_path)

    nominal_supply_v = max((v for _, v in voltage_sources), default=0.0)
    if nominal_supply_v <= 0:
        nominal_supply_v = float(model.get("vdd_nominal_V", 1.1))

    gnd_nominal_v = float(model.get("gnd_voltage_V", 0.0))

    if vdd_attachment in ("pdn_vias", "odb_vias", "real_pdn"):
        if not pdn_via_boxes:
            raise ValueError(
                "model.vdd_attachment requests PDN vias but pdn_geometry.vias_tsv "
                "is missing or empty (export with EXPORT_PDN_VIAS=1 after pdngen).",
            )
        pad_mask = vdd_pad_mask_from_boxes(x_coords, y_coords, nx, ny, pdn_via_boxes)
        print(
            "[1142PDN] VDD attachments: PDN vias from ODB —",
            len(pdn_via_boxes),
            "via box(es); mesh pad nodes",
            int(np.count_nonzero(pad_mask)),
            flush=True,
        )
    else:
        if not voltage_sources:
            raise ValueError(
                "vsrc_loc is missing or produced no sources; set vsrc.loc or use "
                "model.vdd_attachment 'pdn_vias' with pdn_geometry.vias_tsv.",
            )
        pad_mask = vdd_pad_mask(x_coords, y_coords, nx, ny, voltage_sources)

    power_path = resolve_under_root(orfs_root, manifest["report_power_instances_tsv"])
    power_by_instance, sta_voltage = load_instance_power(power_path)
    total_power = sum(power_by_instance.values())

    geom_key = manifest.get("instance_geom_tsv")
    geom_path = resolve_under_root(orfs_root, geom_key) if geom_key else None
    placement = load_instance_geometry(geom_path)
    groups_key = manifest.get("groups")
    rtl_fp_key = manifest.get("rtl_fp")
    groups_path = (
        resolve_under_root(orfs_root, str(groups_key)) if groups_key else None
    )
    rtl_fp_path = (
        resolve_under_root(orfs_root, str(rtl_fp_key)) if rtl_fp_key else None
    )
    dbu_per_um = float(model.get("dbu_per_um", 2000.0))
    instance_to_cluster = load_instance_to_cluster(groups_path)
    fp_regions = load_rtl_fp_regions(rtl_fp_path, dbu_per_um=dbu_per_um)
    cluster_regions = resolve_cluster_regions(instance_to_cluster, placement, fp_regions)
    hard_macro_count = sum(
        1 for g in placement.values() if g.is_macro and bool(g.vdd_pins_um)
    )
    cluster_count = len(cluster_regions)
    print(
        "[1142PDN] instances found:",
        len(placement),
        "| hard macros:",
        hard_macro_count,
        "| clusters:",
        cluster_count,
        flush=True,
    )

    num_nodes = nx * ny
    cell_power = distribute_power_to_cells_watts(
        power_by_instance,
        placement,
        instance_to_cluster,
        cluster_regions,
        x_coords,
        y_coords,
        nx,
        ny,
    )
    apply_uniform_power_remainder(cell_power, total_power, pad_mask)

    v_diff = max(sta_voltage - gnd_nominal_v, 1e-6)
    pkg_vdd, pkg_gnd = package_voltages_heuristic(
        total_power,
        nominal_supply_v,
        gnd_nominal_v,
        sta_voltage,
        r_pkg_ohm,
    )

    big_a = build_two_rail_matrix(laplacian, pad_mask, pad_r_ohm, gnd_ref_s)
    rhs_full = assemble_two_rail_rhs(
        num_nodes,
        cell_power,
        pad_mask,
        pad_r_ohm,
        pkg_vdd,
        pkg_gnd,
        v_diff,
    )
    sol = solve_stationary_linear(big_a, rhs_full)
    vdd_voltages = sol[:num_nodes]

    ir_drop_V = nominal_supply_v - vdd_voltages
    v_worst = float(np.min(vdd_voltages))
    ir_worst = float(np.max(ir_drop_V))
    interior = ~pad_mask
    if np.any(interior):
        ir_avg = float(np.mean(ir_drop_V[interior]))
    else:
        ir_avg = float(np.mean(ir_drop_V))
    pct_ir_worst = (
        100.0 * ir_worst / nominal_supply_v if nominal_supply_v > 1e-30 else float("nan")
    )
    print(
        "[1142PDN] Electrical summary\n"
        f"  Total Power:           {total_power:.6g} W\n"
        f"  Supply Voltage:        {nominal_supply_v:.6g} V (nominal)\n"
        f"  Worst case voltage:    {v_worst:.6g} V\n"
        f"  Average IR drop:       {ir_avg:.6g} V (mean over non-pad mesh nodes)\n"
        f"  Worst case IR drop:    {ir_worst:.6g} V\n"
        f"  Percentage drop:       {pct_ir_worst:.4f} % (worst / nominal supply)\n",
        flush=True,
    )

    pdn_wires_path: Path | None = None
    if pdn_wires_rel:
        pdn_wires_path = resolve_under_root(orfs_root, str(pdn_wires_rel))

    write_mesh_nodes_tsv(
        out,
        x_coords,
        y_coords,
        nx,
        ny,
        vdd_voltages,
        cell_power,
        v_diff,
        nominal_supply_v,
    )
    write_edges_tsv(out, edge_list, x_coords, y_coords, nx)
    write_vsrc_tsv_or_pdn_boxes(
        out, voltage_sources, pdn_via_boxes, nominal_supply_v
    )
    write_instance_regions_tsv(out, placement, cluster_regions)
    write_instance_ir_report(
        out,
        power_by_instance,
        placement,
        x_coords,
        y_coords,
        nx,
        ny,
        vdd_voltages,
        nominal_supply_v,
        observation_layer,
    )

    if pdn_vias_path is not None and pdn_vias_path.is_file():
        copy_pdn_export_to_output(pdn_vias_path, out / "pdn_from_odb_vias.tsv")
    if pdn_wires_path is not None and pdn_wires_path.is_file():
        copy_pdn_export_to_output(pdn_wires_path, out / "pdn_from_odb_wires.tsv")

    meta_path = out / "meta.tsv"
    with meta_path.open("w", encoding="utf-8") as fh:
        fh.write(
            f"core_llx_um\t{core_box.llx}\ncore_lly_um\t{core_box.lly}\n"
            f"core_urx_um\t{core_box.urx}\ncore_ury_um\t{core_box.ury}\n"
        )
        conductance_effective = (
            "pdn_extracted" if use_pdn_extracted_graph else "stripe_mesh"
        )
        fh.write(
            f"mesh_pitch_um\t{mesh_pitch_um}\nRx_ohm\t{rx_ohm}\nRy_ohm\t{ry_ohm}\n"
        )
        fh.write(f"conductance_model\t{conductance_effective}\n")
        if (
            conductance_effective != conductance_model
            and conductance_model in ("pdn_extracted", "pdn", "odb_pdn")
        ):
            fh.write(f"conductance_model_requested\t{conductance_model}\n")
        if use_pdn_extracted_graph:
            fh.write(f"pdn_mesh_nodes\t{nx * ny}\n")
            fh.write(f"pdn_mesh_edges\t{len(edge_list)}\n")
        fh.write(
            f"pad_R_ohm\t{pad_r_ohm}\npackage_R_ohm\t{r_pkg_ohm}\n"
            f"rho_metal_ohm_m\t{rho_ohm_m}\n"
        )
        fh.write(
            f"vdd_nom_V\t{nominal_supply_v}\nvdd_sta_V\t{sta_voltage}\n"
            f"pkg_vdd_V\t{pkg_vdd}\npkg_gnd_V\t{pkg_gnd}\n"
            f"grid_nx\t{nx}\ngrid_ny\t{ny}\n"
        )
        fh.write(
            f"v_min_V\t{float(vdd_voltages.min()):.10g}\n"
            f"v_max_V\t{float(vdd_voltages.max()):.10g}\n"
        )
        fh.write(
            f"ir_drop_max_V\t{float(nominal_supply_v - vdd_voltages.min()):.10g}\n"
        )
        fh.write(f"vdd_attachment\t{vdd_attachment}\n")
        fh.write(f"pdn_via_boxes\t{len(pdn_via_boxes)}\n")
