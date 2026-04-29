#!/usr/bin/env python3
import argparse
import json
import math
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def find_repo_root(manifest_path: Path) -> Path:
    cur = manifest_path.resolve().parent
    while True:
        if (cur / "flow").is_dir() and (cur / "research").is_dir():
            return cur
        if cur.parent == cur:
            raise RuntimeError("Could not locate OpenROAD-flow-scripts root.")
        cur = cur.parent


def resolve_repo_root(manifest_path: Path, root: dict) -> Path:
    repo_override = root.get("repo_root")
    if isinstance(repo_override, str) and repo_override.strip():
        return Path(repo_override).expanduser().resolve()
    return find_repo_root(manifest_path)


def fold_tcl_continuations(text: str) -> str:
    return re.sub(r"\\\n[ \t\r]*", " ", text)


def parse_brace_list(value: str) -> List[str]:
    value = value.strip()
    if value.startswith("{") and value.endswith("}"):
        value = value[1:-1]
    return [tok for tok in value.split() if tok]


def parse_flag_value(cmd: str, flag: str) -> Optional[str]:
    m = re.search(rf"{re.escape(flag)}\s+(\{{[^}}]*\}}|\S+)", cmd)
    return m.group(1) if m else None


def parse_pdn_tcl(tcl_path: Path) -> Tuple[List[dict], List[dict], List[dict]]:
    text = fold_tcl_continuations(tcl_path.read_text(encoding="utf-8"))
    stripes: List[dict] = []
    connects: List[dict] = []
    rings: List[dict] = []

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("add_pdn_stripe"):
            grid = parse_flag_value(line, "-grid")
            layer = parse_flag_value(line, "-layer")
            width = parse_flag_value(line, "-width")
            pitch = parse_flag_value(line, "-pitch")
            offset = parse_flag_value(line, "-offset")
            if not layer or not width or not pitch or not offset:
                continue
            stripes.append(
                {
                    "grid": (grid or "{}").strip("{}"),
                    "layer": layer.strip("{}"),
                    "width": float(width.strip("{}")),
                    "pitch": float(pitch.strip("{}")),
                    "offset": float(offset.strip("{}")),
                    "followpins": "-followpins" in line,
                }
            )
        elif line.startswith("add_pdn_ring"):
            grid = parse_flag_value(line, "-grid")
            layers = parse_flag_value(line, "-layers")
            widths = parse_flag_value(line, "-widths")
            spacings = parse_flag_value(line, "-spacings")
            core_offsets = parse_flag_value(line, "-core_offsets")
            if not layers or not widths or not spacings or not core_offsets:
                continue
            rings.append(
                {
                    "grid": (grid or "{}").strip("{}"),
                    "layers": parse_brace_list(layers),
                    "widths": [float(v) for v in parse_brace_list(widths)],
                    "spacings": [float(v) for v in parse_brace_list(spacings)],
                    "core_offsets": [float(v) for v in parse_brace_list(core_offsets)],
                    "connect_to_pads": "-connect_to_pads" in line,
                }
            )
        elif line.startswith("add_pdn_connect"):
            grid = parse_flag_value(line, "-grid")
            layers = parse_flag_value(line, "-layers")
            if not layers:
                continue
            pair = parse_brace_list(layers)
            if len(pair) < 2:
                continue
            connects.append(
                {
                    "grid": (grid or "{}").strip("{}"),
                    "layer_lo": pair[0],
                    "layer_hi": pair[1],
                }
            )
    return stripes, connects, rings


def value_at(lst: List[float], idx: int) -> float:
    if not lst:
        return 0.0
    if idx < len(lst):
        return lst[idx]
    return lst[-1]


def emit_ring_straps(ring: dict, core: Tuple[float, float, float, float], die: Tuple[float, float, float, float]) -> List[dict]:
    out: List[dict] = []
    c_llx, c_lly, c_urx, c_ury = core
    d_llx, d_lly, d_urx, d_ury = die
    layers = ring["layers"]
    for li, layer in enumerate(layers):
        w = value_at(ring["widths"], li)
        s = value_at(ring["spacings"], li)
        off = value_at(ring["core_offsets"], li)
        if w <= 0.0:
            continue
        pair_pitch = 2.0 * w + s
        outer = off + pair_pitch
        horiz = stripe_is_horizontal(layer)
        rails = []
        if horiz:
            # top pair
            rails.append((c_llx - outer, c_ury + off, c_urx + outer, c_ury + off + w))
            rails.append((c_llx - outer, c_ury + off + w + s, c_urx + outer, c_ury + off + 2.0 * w + s))
            # bottom pair
            rails.append((c_llx - outer, c_lly - off - w, c_urx + outer, c_lly - off))
            rails.append((c_llx - outer, c_lly - off - 2.0 * w - s, c_urx + outer, c_lly - off - w - s))
        else:
            # right pair
            rails.append((c_urx + off, c_lly - outer, c_urx + off + w, c_ury + outer))
            rails.append((c_urx + off + w + s, c_lly - outer, c_urx + off + 2.0 * w + s, c_ury + outer))
            # left pair
            rails.append((c_llx - off - w, c_lly - outer, c_llx - off, c_ury + outer))
            rails.append((c_llx - off - 2.0 * w - s, c_lly - outer, c_llx - off - w - s, c_ury + outer))

        for raw in rails:
            clipped = clip_rect(raw, die)
            if not clipped:
                continue
            out.append(
                {
                    "kind": "ring",
                    "grid": ring["grid"],
                    "layer": layer,
                    "width": w,
                    "pitch": pair_pitch,
                    "offset": off,
                    "followpins": False,
                    "horiz": 1 if horiz else 0,
                    "rect": clipped,
                }
            )
    return out


def metal_rank(layer: str) -> int:
    runs = re.findall(r"\d+", layer)
    return int(runs[-1]) if runs else -1


def stripe_is_horizontal(layer: str) -> bool:
    rank = metal_rank(layer)
    return rank > 0 and (rank % 2 == 1)


def stripe_centers_1d(offset: float, pitch: float, half_w: float, lo: float, hi: float) -> List[float]:
    if pitch <= 0.0:
        return []
    out: List[float] = []
    k_min = math.floor((lo - offset + half_w) / pitch) - 4
    k_max = math.ceil((hi - offset - half_w) / pitch) + 4
    for k in range(int(k_min), int(k_max) + 1):
        c = offset + k * pitch
        if c - half_w >= lo - 1e-9 and c + half_w <= hi + 1e-9:
            out.append(c)
    return out


def clip_rect(a: Tuple[float, float, float, float], b: Tuple[float, float, float, float]) -> Optional[Tuple[float, float, float, float]]:
    llx = max(a[0], b[0])
    lly = max(a[1], b[1])
    urx = min(a[2], b[2])
    ury = min(a[3], b[3])
    if urx <= llx or ury <= lly:
        return None
    return (llx, lly, urx, ury)


def generate(manifest_path: Path) -> Path:
    root = json.loads(manifest_path.read_text(encoding="utf-8"))
    repo = resolve_repo_root(manifest_path, root)
    core = tuple(float(v) for v in root["layout"]["core"])
    die = tuple(float(v) for v in root["layout"]["die"])
    extent = die if (die[2] > die[0] and die[3] > die[1]) else core
    pdn_tcl = repo / root["tech"]["pdn_tcl"]

    stripes, connects, rings = parse_pdn_tcl(pdn_tcl)
    out_path = repo / "research/out/pdn_tcl_physical.tsv"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with out_path.open("w", encoding="utf-8") as f:
        f.write(
            "# pdn_tcl_physical: literal add_pdn_stripe rectangles (um) clipped to layout.core; "
            "add_pdn_connect has no via coordinates in Tcl.\n"
        )
        f.write("# core_llx\tcore_lly\tcore_urx\tcore_ury\n")
        f.write(f"#\t{core[0]:.6f}\t{core[1]:.6f}\t{core[2]:.6f}\t{core[3]:.6f}\n")
        f.write("# die_llx\tdie_lly\tdie_urx\tdie_ury\n")
        f.write(f"#\t{die[0]:.6f}\t{die[1]:.6f}\t{die[2]:.6f}\t{die[3]:.6f}\n")
        f.write("kind\tgrid\tlayer_lo\tlayer_hi\twidth_um\tpitch_um\toffset_um\tfollowpins\thoriz\tllx\tlly\turx\tury\n")

        elx, ely, eux, euy = extent
        all_straps: List[dict] = []
        for s in stripes:
            if s["width"] <= 0.0 or s["pitch"] <= 0.0:
                continue
            half = 0.5 * s["width"]
            horiz = stripe_is_horizontal(s["layer"])
            if horiz:
                centers = stripe_centers_1d(s["offset"], s["pitch"], half, ely, euy)
                for yc in centers:
                    raw = (elx, yc - half, eux, yc + half)
                    clipped = clip_rect(raw, core)
                    if not clipped:
                        continue
                    all_straps.append(
                        {
                            "kind": "strap",
                            "grid": s["grid"],
                            "layer": s["layer"],
                            "width": s["width"],
                            "pitch": s["pitch"],
                            "offset": s["offset"],
                            "followpins": s["followpins"],
                            "horiz": 1,
                            "rect": clipped,
                        }
                    )
            else:
                centers = stripe_centers_1d(s["offset"], s["pitch"], half, elx, eux)
                for xc in centers:
                    raw = (xc - half, ely, xc + half, euy)
                    clipped = clip_rect(raw, core)
                    if not clipped:
                        continue
                    all_straps.append(
                        {
                            "kind": "strap",
                            "grid": s["grid"],
                            "layer": s["layer"],
                            "width": s["width"],
                            "pitch": s["pitch"],
                            "offset": s["offset"],
                            "followpins": s["followpins"],
                            "horiz": 0,
                            "rect": clipped,
                        }
                    )

        for ring in rings:
            all_straps.extend(emit_ring_straps(ring, core, die))

        for s in all_straps:
            llx, lly, urx, ury = s["rect"]
            f.write(
                f"{s['kind']}\t{s['grid']}\t{s['layer']}\t\t{s['width']:.6f}\t{s['pitch']:.6f}\t{s['offset']:.6f}\t"
                f"{1 if s['followpins'] else 0}\t{s['horiz']}\t"
                f"{llx:.6f}\t{lly:.6f}\t{urx:.6f}\t{ury:.6f}\n"
            )

        for c in connects:
            f.write(f"connect\t{c['grid']}\t{c['layer_lo']}\t{c['layer_hi']}\t\t\t\t\t\t\t\t\t\n")

    return out_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate research/out/pdn_tcl_physical.tsv from manifest PDN Tcl.")
    parser.add_argument("manifest", nargs="?", default="mempool.json", help="Path to manifest json")
    args = parser.parse_args()
    manifest = Path(args.manifest).resolve()
    if not manifest.is_file():
        parser.error(f"manifest not found: {manifest}")
    out_path = generate(manifest)
    print(f"[pdn-tcl-physical] wrote {out_path} (literal Tcl straps + connects)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
