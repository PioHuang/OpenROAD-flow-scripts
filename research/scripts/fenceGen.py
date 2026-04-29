#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""
Inject DEF 5.8 REGIONS (+ TYPE FENCE) from RTLMP floorplan (root.fp.txt) and
link each matching GROUP with + REGION <id>, for DREAMPlace-style fence flow.

Typical inputs come from research/mempool.json (paths relative to ORFS root).

Example:
  cd OpenROAD-flow-scripts
  ./research/scripts/fenceGen.sh \\
    research/mempool.json \\
    flow/results/nangate45/mempool_group/base/2_floorplan.def \\
    flow/results/nangate45/mempool_group/base/2_floorplan_fenced.def
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, List, Optional, Sequence, Set, Tuple


def orfs_root_from_this_file() -> str:
    return os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    )


def resolve_path(p: str, root: str) -> str:
    p = os.path.expanduser(p)
    if os.path.isabs(p):
        return p
    return os.path.normpath(os.path.join(root, p))


def parse_diearea_dbu(lines: List[str]) -> Optional[Tuple[int, int, int, int]]:
    pat = re.compile(
        r"DIEAREA\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s*;"
    )
    for line in lines[:8000]:
        m = pat.search(line)
        if m:
            return int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    return None


def parse_root_fp(
    path: str,
    min_span_dbu: int,
    max_cover_frac: float,
    die: Optional[Tuple[int, int, int, int]],
) -> Dict[str, Tuple[int, int, int, int]]:
    """cluster_name -> (llx, lly, urx, ury) in DEF DBU."""
    out: Dict[str, Tuple[int, int, int, int]] = {}
    die_area = None
    if die is not None:
        xl, yl, xh, yh = die
        die_area = max(0, xh - xl) * max(0, yh - yl)

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 5:
                continue
            try:
                llx, lly, w, h = (int(parts[-4]), int(parts[-3]), int(parts[-2]), int(parts[-1]))
            except ValueError:
                continue
            name = " ".join(parts[:-4]).strip()
            if not name:
                continue
            if w < min_span_dbu or h < min_span_dbu:
                continue
            urx = llx + w
            ury = lly + h
            if die is not None and die_area and die_area > 0 and max_cover_frac < 1.0:
                box_area = max(0, urx - llx) * max(0, ury - lly)
                if box_area >= max_cover_frac * die_area:
                    continue
            out[name] = (llx, lly, urx, ury)
    return out


def collect_group_head_names(lines: List[str], groups_line_idx: int) -> Set[str]:
    names: Set[str] = set()
    i = groups_line_idx + 1
    buf: List[str] = []
    while i < len(lines):
        ln = lines[i]
        if ln.strip() == "END GROUPS":
            break
        buf.append(ln)
        if ln.rstrip().endswith(";"):
            first = buf[0].strip()
            if first.startswith("-"):
                toks = first[1:].strip().split()
                if toks:
                    names.add(toks[0])
            buf = []
        i += 1
    return names


def find_groups_block(lines: List[str]) -> Tuple[Optional[int], Optional[int]]:
    groups_idx = None
    for i, ln in enumerate(lines):
        s = ln.strip()
        if s.startswith("GROUPS ") and s.endswith(";"):
            groups_idx = i
            break
    if groups_idx is None:
        return None, None
    for j in range(groups_idx + 1, len(lines)):
        if lines[j].strip() == "END GROUPS":
            return groups_idx, j
    raise SystemExit("found GROUPS header but no END GROUPS")


def parse_def_components(lines: List[str]) -> Dict[str, str]:
    components: Dict[str, str] = {}
    in_components = False
    for ln in lines:
        s = ln.strip()
        if s.startswith("COMPONENTS ") and s.endswith(";"):
            in_components = True
            continue
        if in_components and s == "END COMPONENTS":
            break
        if in_components and s.startswith("- "):
            m = re.match(r"-\s+(\S+)\s+\S+", s)
            if m:
                name = m.group(1)
                status = "UNKNOWN"
                sm = re.search(r"\+\s+(FIXED|PLACED|UNPLACED|COVER)\b", s)
                if sm:
                    status = sm.group(1)
                components[name] = status
    return components


def parse_rtlmp_instance_to_cluster(path: str) -> Dict[str, List[str]]:
    cluster_to_insts: Dict[str, List[str]] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            s = raw.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            if len(parts) < 4:
                continue
            inst = parts[0]
            # Format: instance cluster_id cluster_name depth. Keep this tolerant
            # in case cluster names ever contain whitespace.
            cluster = " ".join(parts[2:-1]).strip()
            if not cluster:
                continue
            cluster_to_insts.setdefault(cluster, []).append(inst)
    return cluster_to_insts


def filter_groups_with_movable_members(
    cluster_to_members: Dict[str, List[str]],
    component_status: Dict[str, str],
) -> Tuple[Dict[str, List[str]], int, int]:
    filtered: Dict[str, List[str]] = {}
    dropped_fixed_only = 0
    dropped_missing_only = 0
    for cluster, members in cluster_to_members.items():
        valid_members = [m for m in members if m in component_status]
        movable_members = [
            m for m in valid_members if component_status[m] not in {"FIXED", "COVER"}
        ]
        if not valid_members:
            dropped_missing_only += 1
            continue
        if not movable_members:
            dropped_fixed_only += 1
            continue
        filtered[cluster] = movable_members
    if dropped_missing_only:
        print(
            f"warning: skipped {dropped_missing_only} RTLMP clusters with no DEF components",
            file=sys.stderr,
        )
    if dropped_fixed_only:
        print(
            f"info: skipped {dropped_fixed_only} fixed-only RTLMP clusters",
            file=sys.stderr,
        )
    return filtered, dropped_fixed_only, dropped_missing_only


def has_regions_before(lines: List[str], groups_line_idx: int, lookback: int) -> bool:
    lo = max(0, groups_line_idx - lookback)
    for j in range(lo, groups_line_idx):
        if lines[j].strip().startswith("REGIONS"):
            return True
    return False


def build_regions_block(
    cluster_to_bbox: Dict[str, Tuple[int, int, int, int]],
    group_names: Set[str],
) -> Tuple[List[str], Dict[str, str]]:
    """Return (REGIONS ... END REGIONS lines, cluster_name -> region_id)."""
    wanted = sorted(n for n in group_names if n in cluster_to_bbox)
    rid_map: Dict[str, str] = {}
    out_lines: List[str] = []
    for idx, name in enumerate(wanted):
        rid = f"rtlmp_fn_{idx}"
        rid_map[name] = rid
        llx, lly, urx, ury = cluster_to_bbox[name]
        out_lines.append(
            f"   - {rid} ( {llx} {lly} ) ( {urx} {ury} ) + TYPE FENCE ;\n"
        )
    n = len(out_lines)
    block_lines = [f"REGIONS {n} ;\n"] + out_lines + ["END REGIONS\n", "\n"]
    return block_lines, rid_map


def build_clean_groups_block(
    cluster_to_members: Dict[str, List[str]],
    rid_map: Dict[str, str],
    wrap_columns: int = 500,
) -> Tuple[List[str], int]:
    group_names = [name for name in sorted(rid_map) if cluster_to_members.get(name)]
    out = [f"GROUPS {len(group_names)} ;\n"]
    for group_name in group_names:
        members = cluster_to_members[group_name]
        rid = rid_map[group_name]
        line = f"   - {group_name}"
        for member in members:
            token = f" {member}"
            if len(line) + len(token) > wrap_columns:
                out.append(line + "\n")
                line = "     " + member
            else:
                line += token
        out.append(line + "\n")
        out.append(f"     + REGION {rid} ;\n")
    out.append("END GROUPS\n")
    return out, len(group_names)


def rewrite_groups(
    lines: List[str],
    groups_line_idx: int,
    rid_map: Dict[str, str],
    drop_unmapped_groups: bool = False,
) -> Tuple[List[str], int, int]:
    out: List[str] = []
    out.extend(lines[: groups_line_idx + 1])
    i = groups_line_idx + 1
    buf: List[str] = []
    kept_groups = 0
    dropped_groups = 0
    while i < len(lines):
        ln = lines[i]
        if ln.strip() == "END GROUPS":
            if buf:
                chunk, keep = _flush_group(buf, rid_map, drop_unmapped_groups)
                if keep:
                    out.extend(chunk)
                    kept_groups += 1
                else:
                    dropped_groups += 1
                buf = []
            out.append(ln)
            i += 1
            break
        buf.append(ln)
        if ln.rstrip().endswith(";"):
            chunk, keep = _flush_group(buf, rid_map, drop_unmapped_groups)
            if keep:
                out.extend(chunk)
                kept_groups += 1
            else:
                dropped_groups += 1
            buf = []
        i += 1
    out.extend(lines[i:])
    return out, kept_groups, dropped_groups


def _flush_group(
    raw_lines: List[str], rid_map: Dict[str, str], drop_unmapped_groups: bool
) -> Tuple[List[str], bool]:
    if not raw_lines:
        return [], False
    joined = " ".join(x.strip() for x in raw_lines)
    if "+ REGION" in joined:
        return raw_lines, True
    first = raw_lines[0].strip()
    if not first.startswith("-"):
        return raw_lines, True
    toks = first[1:].strip().split()
    if not toks:
        return raw_lines, True
    gname = toks[0]
    rid = rid_map.get(gname)
    if rid is None:
        return ([], False) if drop_unmapped_groups else (raw_lines, True)
    last = raw_lines[-1].rstrip()
    if not last.endswith(";"):
        return raw_lines, True
    body = last[:-1].rstrip()
    prefix = raw_lines[:-1]
    indent = "     "
    return prefix + [body + "\n", f"{indent}+ REGION {rid} ;\n"], True


def rewrite_groups_count(group_block_lines: List[str], kept_groups: int) -> List[str]:
    if not group_block_lines:
        return group_block_lines
    hdr = group_block_lines[0]
    m = re.match(r"(\s*GROUPS\s+)(\d+)(\s*;\s*)$", hdr)
    if not m:
        return group_block_lines
    group_block_lines[0] = f"{m.group(1)}{kept_groups}{m.group(3)}\n"
    return group_block_lines


def load_json_paths(json_path: str, root: str) -> Tuple[str, Optional[str]]:
    with open(json_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    rtl_fp = cfg.get("rtl_fp")
    if not rtl_fp:
        raise SystemExit(f"{json_path}: missing key 'rtl_fp'")
    groups = cfg.get("groups")
    groups_path = resolve_path(groups, root) if groups else None
    return resolve_path(rtl_fp, root), groups_path


def main() -> None:
    ap = argparse.ArgumentParser(
        description="fenceGen: inject RTLMP fence REGIONS + REGION links into a DEF."
    )
    ap.add_argument(
        "--json",
        metavar="PATH",
        help="research/mempool.json (uses rtl_fp path relative to ORFS root)",
    )
    ap.add_argument(
        "--fp",
        metavar="PATH",
        help="Override root.fp.txt path (absolute or relative to ORFS root)",
    )
    ap.add_argument(
        "--groups",
        metavar="PATH",
        help=(
            "Override rtlmp_instance_to_cluster.txt path. Required for "
            "--clean-groups-from-rtlmp when --json does not provide 'groups'."
        ),
    )
    ap.add_argument(
        "--def-in",
        dest="def_in",
        metavar="PATH",
        required=True,
        help="Input DEF (e.g. 2_floorplan.def)",
    )
    ap.add_argument(
        "--def-out",
        dest="def_out",
        metavar="PATH",
        required=True,
        help="Output DEF path",
    )
    ap.add_argument(
        "--root",
        metavar="DIR",
        default=None,
        help="OpenROAD-flow-scripts root (default: infer from this script)",
    )
    ap.add_argument(
        "--min-span-dbu",
        type=int,
        default=2,
        help="Skip fp rows with width or height below this (DBU). Default 2.",
    )
    ap.add_argument(
        "--max-cover-frac",
        type=float,
        default=0.92,
        help="Skip fp bbox if area >= this fraction of die area (filters die-filling rows).",
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="Allow injection even if a REGIONS block already appears above GROUPS.",
    )
    ap.add_argument(
        "--drop-unmapped-groups",
        action="store_true",
        help=(
            "Drop GROUP entries that do not map to an injected fence REGION. "
            "Use this only when you intentionally want a fence-only GROUPS section."
        ),
    )
    ap.add_argument(
        "--clean-groups-from-rtlmp",
        action="store_true",
        help=(
            "Remove the original DEF GROUPS block and generate a clean fence-only "
            "GROUPS block from rtlmp_instance_to_cluster.txt."
        ),
    )
    args = ap.parse_args()

    root = args.root or orfs_root_from_this_file()
    def_in = resolve_path(args.def_in, root)
    def_out = resolve_path(args.def_out, root)

    groups_path = resolve_path(args.groups, root) if args.groups else None
    if args.fp:
        fp_path = resolve_path(args.fp, root)
    elif args.json:
        fp_path, json_groups_path = load_json_paths(resolve_path(args.json, root), root)
        if groups_path is None:
            groups_path = json_groups_path
    else:
        ap.error("need --json or --fp for RTLMP floorplan text")

    if not os.path.isfile(def_in):
        raise SystemExit(f"input DEF not found: {def_in}")
    if not os.path.isfile(fp_path):
        raise SystemExit(f"RTLMP fp file not found: {fp_path}")
    if args.clean_groups_from_rtlmp:
        if not groups_path:
            raise SystemExit("--clean-groups-from-rtlmp requires --groups or JSON key 'groups'")
        if not os.path.isfile(groups_path):
            raise SystemExit(f"RTLMP groups file not found: {groups_path}")

    with open(def_in, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    die = parse_diearea_dbu(lines)
    fp_map = parse_root_fp(
        fp_path,
        min_span_dbu=args.min_span_dbu,
        max_cover_frac=args.max_cover_frac,
        die=die,
    )

    groups_idx, groups_end_idx = find_groups_block(lines)
    if groups_idx is None or groups_end_idx is None:
        raise SystemExit("no GROUPS section found in DEF")

    if not args.force and has_regions_before(lines, groups_idx, lookback=8000):
        raise SystemExit(
            "DEF already contains REGIONS near GROUPS; use --force to inject anyway."
        )

    if args.clean_groups_from_rtlmp:
        component_status = parse_def_components(lines)
        raw_rtlmp_groups = parse_rtlmp_instance_to_cluster(groups_path)
        rtlmp_groups, dropped_fixed_only, dropped_missing_only = filter_groups_with_movable_members(
            raw_rtlmp_groups, component_status
        )
        group_names = set(rtlmp_groups)
    else:
        rtlmp_groups = {}
        dropped_fixed_only = 0
        dropped_missing_only = 0
        group_names = collect_group_head_names(lines, groups_idx)
    region_lines, rid_map = build_regions_block(fp_map, group_names)
    if not rid_map:
        print(
            "warning: no intersection between DEF GROUP heads and root.fp.txt clusters "
            "(check paths or naming). Writing DEF unchanged (no REGIONS added).",
            file=sys.stderr,
        )
        new_lines = lines
        kept_groups = len(group_names)
        dropped_groups = 0
    else:
        if args.clean_groups_from_rtlmp:
            clean_group_lines, kept_groups = build_clean_groups_block(rtlmp_groups, rid_map)
            dropped_groups = len(group_names) - kept_groups
            new_lines = (
                lines[:groups_idx]
                + region_lines
                + clean_group_lines
                + lines[groups_end_idx + 1 :]
            )
        else:
            new_lines = lines[:groups_idx] + region_lines + lines[groups_idx:]
            groups_idx += len(region_lines)
            new_lines, kept_groups, dropped_groups = rewrite_groups(
                new_lines,
                groups_idx,
                rid_map,
                drop_unmapped_groups=args.drop_unmapped_groups,
            )
            if args.drop_unmapped_groups:
                group_start = groups_idx
                group_end = None
                for j in range(group_start + 1, len(new_lines)):
                    if new_lines[j].strip() == "END GROUPS":
                        group_end = j
                        break
                if group_end is not None:
                    new_lines[group_start : group_end + 1] = rewrite_groups_count(
                        new_lines[group_start : group_end + 1], kept_groups
                    )

    os.makedirs(os.path.dirname(def_out) or ".", exist_ok=True)
    with open(def_out, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(new_lines)

    print(
        f"wrote {def_out}\n"
        f"  fp={fp_path}\n"
        f"  groups={groups_path if groups_path else '<original DEF GROUPS>'}\n"
        f"  regions={len(rid_map)} linked groups (of {len(group_names)} DEF group heads, "
        f"{len(fp_map)} fp clusters)\n"
        f"  groups_kept={kept_groups} groups_dropped={dropped_groups}\n"
        f"  fixed_only_clusters_skipped={dropped_fixed_only} "
        f"missing_component_clusters_skipped={dropped_missing_only}"
    )


if __name__ == "__main__":
    main()
