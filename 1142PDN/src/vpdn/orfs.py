import json
import math
from pathlib import Path
from typing import Dict, List, Tuple

from vpdn.model import HardLoad, HardMacro, Layer, Problem, Rect, SoftCluster, Source


def split_tab(line: str) -> List[str]:
    return line.rstrip("\n").split("\t")


def split_ws(line: str) -> List[str]:
    return line.split()


def layer_rank(name: str) -> int:
    run = -1
    best = -1
    for ch in name:
        if ch.isdigit():
            if run < 0:
                run = 0
            run = run * 10 + int(ch)
            best = run
        else:
            run = -1
    return best


def median(vals: List[float]) -> float:
    if not vals:
        return 0.0
    vals = sorted(vals)
    return vals[len(vals) // 2]


def find_repo_root(manifest: Path) -> Path:
    cur = manifest.resolve().parent
    while True:
        if (cur / "flow").is_dir() and (cur / "research").is_dir():
            return cur
        if cur.parent == cur:
            raise RuntimeError("Could not locate OpenROAD-flow-scripts root.")
        cur = cur.parent


def read_json(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def parse_set_rc(path: Path) -> Dict[str, float]:
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if "set_layer_rc" not in line:
                continue
            tok = split_ws(line)
            layer = ""
            r = 0.0
            for i in range(len(tok) - 1):
                if tok[i] == "-layer":
                    layer = tok[i + 1]
                elif tok[i] == "-resistance":
                    r = float(tok[i + 1])
            if layer and r > 0.0:
                out[layer] = r
    return out


def parse_pdn_tcl(path: Path, r_by_layer: Dict[str, float], min_pitch_um: float) -> List[Layer]:
    obs = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            tok = split_tab(line)
            if len(tok) < 13 or tok[0] != "strap":
                continue
            layer = tok[2]
            if layer not in r_by_layer:
                continue
            if layer not in obs:
                obs[layer] = {"pitch": [], "width": [], "h": 0}
            obs[layer]["pitch"].append(float(tok[5]))
            obs[layer]["width"].append(float(tok[4]))
            obs[layer]["h"] += 1 if int(tok[8]) else -1

    layers = []
    for name, o in obs.items():
        pitch = median(o["pitch"])
        width = median(o["width"])
        if pitch < min_pitch_um or width <= 0.0:
            continue
        layers.append(
            Layer(
                name=name,
                rank=layer_rank(name),
                is_h=o["h"] >= 0,
                pitch_um=pitch,
                width_um=width,
                r_ohm_per_um=r_by_layer[name],
            )
        )
    layers.sort(key=lambda x: x.rank)
    return layers


def parse_sources(path: Path) -> List[Source]:
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            tok = split_tab(line)
            if len(tok) < 5 or tok[0] == "llx_um":
                continue
            layer = tok[5] if len(tok) > 5 else ""
            llx = float(tok[0])
            lly = float(tok[1])
            urx = float(tok[2])
            ury = float(tok[3])
            cx = 0.5 * (llx + urx)
            cy = 0.5 * (lly + ury)
            out.append(
                Source(
                    layer=layer,
                    # Match research/src/pdn_ir.cpp source_rect_centers_only():
                    # treat each VDD source by the geometric center of the ODB pin box.
                    rect=Rect(cx, cy, cx, cy),
                    voltage_v=float(tok[4]),
                )
            )
    return out


def choose_grid_pitch_um(layers: List[Layer]) -> float:
    if layers:
        # Technology-agnostic choice: virtual-grid pitch follows the top retained
        # PDN routing layer (largest routing rank), not any specific metal name.
        return max(layers, key=lambda l: l.rank).pitch_um
    raise RuntimeError("No PDN layers available to infer virtual-grid pitch.")


def parse_power(path: Path) -> Dict[str, float]:
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            tok = split_tab(line)
            if len(tok) < 6 or tok[0] == "instance":
                continue
            out[tok[0]] = float(tok[5])
    return out


def parse_groups(path: Path) -> Dict[str, Tuple[int, str]]:
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            tok = split_ws(line)
            if len(tok) < 3 or tok[0] == "instance":
                continue
            out[tok[0]] = (int(tok[1]), tok[2])
    return out


def parse_geom(path: Path):
    geom = {}
    with open(path, "r", encoding="utf-8") as f:
        header = None
        col = {}
        for line in f:
            if not line.strip():
                continue
            tok = split_tab(line)
            if header is None:
                header = tok
                col = {name: i for i, name in enumerate(header)}
                continue
            if tok[0] == "instance":
                continue
            if "master" in col:
                # db_instances.tsv schema from research/scripts/extract_floorplan_db.tcl
                req = ["instance", "is_macro", "llx_um", "lly_um", "urx_um", "ury_um", "cx_um", "cy_um", "area_um2"]
                if any(k not in col for k in req):
                    continue
                name = tok[col["instance"]]
                is_macro = int(tok[col["is_macro"]])
                rect = Rect(
                    float(tok[col["llx_um"]]),
                    float(tok[col["lly_um"]]),
                    float(tok[col["urx_um"]]),
                    float(tok[col["ury_um"]]),
                )
                cx = float(tok[col["cx_um"]])
                cy = float(tok[col["cy_um"]])
                area = float(tok[col["area_um2"]])
                pin_x = tok[col["pg_pin_x_um"]] if "pg_pin_x_um" in col and col["pg_pin_x_um"] < len(tok) else ""
                pin_y = tok[col["pg_pin_y_um"]] if "pg_pin_y_um" in col and col["pg_pin_y_um"] < len(tok) else ""
            else:
                # Legacy report_power geom schema.
                if len(tok) < 10:
                    continue
                name = tok[0]
                is_macro = int(tok[2])
                rect = Rect(float(tok[3]), float(tok[4]), float(tok[5]), float(tok[6]))
                cx = float(tok[7])
                cy = float(tok[8])
                area = float(tok[9])
                pin_x = tok[10] if len(tok) > 10 else ""
                pin_y = tok[11] if len(tok) > 11 else ""
            row = geom.setdefault(
                name,
                {
                    "is_macro": is_macro,
                    "rect": rect,
                    "cx": cx,
                    "cy": cy,
                    "area": area,
                    "pins": [],
                },
            )
            if pin_x and pin_y:
                row["pins"].append((float(pin_x), float(pin_y)))
    return geom


class Knapsack:
    def __init__(self, items: List[Tuple[float, float]]):
        pairs = [(i, max(a, 1e-12)) for i, a in items if i > 0.0 and a > 0.0]
        pairs.sort(key=lambda x: x[0] / x[1], reverse=True)
        self.items = pairs
        self.pref_a = [0.0]
        self.pref_i = [0.0]
        for cur_i, cur_a in pairs:
            self.pref_a.append(self.pref_a[-1] + cur_a)
            self.pref_i.append(self.pref_i[-1] + cur_i)

    def query(self, cap_um2: float) -> float:
        if cap_um2 <= 0.0 or not self.items:
            return 0.0
        lo = 0
        hi = len(self.pref_a) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.pref_a[mid] <= cap_um2:
                lo = mid
            else:
                hi = mid - 1
        full = lo
        out = self.pref_i[full]
        if full < len(self.items):
            rem = cap_um2 - self.pref_a[full]
            if rem > 0.0:
                item_i, item_a = self.items[full]
                out += rem / item_a * item_i
        return out


def parse_rtl_fp(path: Path, dbu_per_um: float = 2000.0) -> Dict[str, Rect]:
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            tok = split_ws(line)
            if len(tok) < 5:
                continue
            name = tok[0]
            x = float(tok[1]) / dbu_per_um
            y = float(tok[2]) / dbu_per_um
            w = float(tok[3]) / dbu_per_um
            h = float(tok[4]) / dbu_per_um
            out[name] = Rect(x, y, x + w, y + h)
    return out


def infer_assumed_cell_area_um2(geom: dict) -> float:
    vals = [g["area"] for g in geom.values() if g["area"] > 0.0]
    return median(vals) if vals else 1.0


def infer_packing_util(core: Rect, geom: dict) -> float:
    core_area = core.area_um2()
    if core_area <= 0.0:
        return 1.0
    total = sum(g["area"] for g in geom.values() if g["area"] > 0.0)
    util = total / core_area
    util = min(max(util, 0.0), 1.0)
    return util if util > 0.0 else 1.0


def build_loads(
    power: Dict[str, float],
    groups: Dict[str, Tuple[int, str]],
    geom: dict,
    fp_boxes: Dict[str, Rect],
    core: Rect,
) -> Tuple[List[HardLoad], List[SoftCluster], List[HardMacro]]:
    hard = []
    hard_macros: List[HardMacro] = []
    missing_macro_pins: List[str] = []
    soft_acc = {}
    assumed_area = infer_assumed_cell_area_um2(geom)
    packing_util = infer_packing_util(core, geom)

    for inst, cur in power.items():
        if cur <= 0.0 or inst not in geom:
            continue
        g = geom[inst]
        if g["is_macro"]:
            pins = g["pins"]
            if not pins:
                missing_macro_pins.append(inst)
                continue
            share = cur / float(len(pins))
            hard_macros.append(
                HardMacro(
                    name=inst,
                    rect=g["rect"],
                    pin_count=len(pins),
                    total_current_a=cur,
                )
            )
            for idx, (x, y) in enumerate(pins):
                hard.append(HardLoad(name=f"{inst}#{idx}", x_um=x, y_um=y, current_a=share))
            continue

        if inst not in groups:
            continue
        cluster_id, cluster_name = groups[inst]
        row = soft_acc.setdefault(
            cluster_id,
            {
                "name": cluster_name,
                "instance_count": 0,
                "n_std": 0,
                "observed": 0.0,
                "items": [],
            },
        )
        row["instance_count"] += 1
        row["n_std"] += 1
        row["observed"] += cur
        area = g["area"] if g["area"] > 0.0 else assumed_area
        row["items"].append((cur, max(area, 1e-12)))

    soft = []
    for cluster_id, row in soft_acc.items():
        if row["n_std"] == 0:
            continue
        rect = fp_boxes.get(row["name"], Rect(0.0, 0.0, 0.0, 0.0))
        kn = Knapsack(row["items"])
        cap_um2 = rect.area_um2() * max(0.0, packing_util)
        i_pack = kn.query(cap_um2) if cap_um2 > 0.0 else row["observed"]
        worst_a = max(i_pack, row["observed"])
        soft.append(
            SoftCluster(
                name=row["name"],
                rect=rect,
                total_a=worst_a,
                observed_a=row["observed"],
                worst_a=worst_a,
                cluster_id=cluster_id,
                instance_count=row["instance_count"],
                items=row["items"],
            )
        )
    if missing_macro_pins:
        names = ", ".join(sorted(missing_macro_pins))
        raise RuntimeError(f"Missing hard-macro P/G pin data in instance_geom_tsv for: {names}")
    return hard, soft, hard_macros


def choose_obs_layer(layers: List[Layer]) -> str:
    for layer in layers:
        if layer.rank > 1:
            return layer.name
    return layers[0].name


def load_problem(manifest_path: str, grid_intv: int = 2, pad_r_ohm: float = 10e-3, min_pitch_um: float = 5.0) -> Problem:
    manifest = Path(manifest_path).resolve()
    repo = find_repo_root(manifest)
    root = read_json(manifest)

    core = Rect(*root["layout"]["core"])
    r_by_layer = parse_set_rc(repo / root["tech"]["set_rc_tcl"])
    layers = parse_pdn_tcl(repo / "research/out/pdn_tcl_physical.tsv", r_by_layer, min_pitch_um)
    sources = parse_sources(repo / root["psm_vsrc_boxes_file"])
    grid_pitch_um = choose_grid_pitch_um(layers)
    power = parse_power(repo / root["report_power_instances_tsv"])
    groups = parse_groups(repo / root["groups"])
    geom = parse_geom(repo / root["instance_geom_tsv"])
    fp_boxes = parse_rtl_fp(repo / root["rtl_fp"])
    hard, soft, hard_macros = build_loads(power, groups, geom, fp_boxes, core)

    vdd = median([s.voltage_v for s in sources]) if sources else 1.0
    return Problem(
        name=root.get("name", manifest.stem),
        core=core,
        layers=layers,
        sources=sources,
        hard=hard,
        hard_macros=hard_macros,
        soft=soft,
        vdd=vdd,
        grid_pitch_um=grid_pitch_um,
        pad_r_ohm=pad_r_ohm,
        grid_intv=grid_intv,
        obs_layer=choose_obs_layer(layers),
    )
