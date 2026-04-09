#!/usr/bin/env python3

import argparse
import csv
import os
import re
import subprocess
import tempfile
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patches as patches


RULE = "-" * 80


def _flow_dir() -> Path:
    """This script lives in flow/designs/<platform>/<design>/placepng.py → parent⁴ == flow/."""
    return Path(__file__).resolve().parent.parent.parent.parent


DEFAULT_MEMBERSHIP = (
    _flow_dir() / "reports/nangate45/mempool_group/base/rtlmp_instance_to_cluster.txt"
)


def fail(msg: str):
    raise RuntimeError(msg)


def parse_cluster_membership_hierarchy(path: Path):
    groups = []
    current = None
    reading_insts = False

    with path.open("r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.rstrip("\n")
            if not line:
                continue
            if line == RULE:
                reading_insts = False
                continue
            if line.startswith("RTLMP cluster hierarchy dump"):
                continue
            if line.startswith("TOP_LEVEL_GROUPS:"):
                continue
            if line.startswith("GROUP: "):
                if current is not None:
                    groups.append(current)
                current = {
                    "group_name": line[len("GROUP: ") :],
                    "parent": "",
                    "type": "",
                    "depth": 0,
                    "child_group_count": 0,
                    "direct_insts": [],
                }
                reading_insts = False
                continue
            if current is None:
                continue
            if line.startswith("PARENT: "):
                current["parent"] = line[len("PARENT: ") :]
            elif line.startswith("TYPE: "):
                current["type"] = line[len("TYPE: ") :]
            elif line.startswith("DEPTH: "):
                current["depth"] = int(line[len("DEPTH: ") :])
            elif line.startswith("CHILD_GROUP_COUNT: "):
                current["child_group_count"] = int(line[len("CHILD_GROUP_COUNT: ") :])
            elif line == "DIRECT_INSTS:":
                reading_insts = True
            elif reading_insts and line.startswith("  "):
                current["direct_insts"].append(line.strip())

    if current is not None:
        groups.append(current)

    leaf_groups = [
        group
        for group in groups
        if group["child_group_count"] == 0 and group["direct_insts"]
    ]

    inst_to_group = {}
    duplicate_insts = set()
    for group in leaf_groups:
        for inst in group["direct_insts"]:
            if inst in inst_to_group and inst_to_group[inst] != group["group_name"]:
                duplicate_insts.add(inst)
                continue
            inst_to_group[inst] = group["group_name"]

    return leaf_groups, inst_to_group, duplicate_insts


def parse_instance_to_cluster_txt(path: Path):
    """
    Parse reports/.../rtlmp_instance_to_cluster.txt from rtlmp_extract.tcl:
    # instance cluster_id cluster_name depth
    <inst_name> <gid> <gname...> <depth>
    """
    inst_to_group = {}
    duplicate_insts = set()
    with path.open("r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                int(parts[-1])  # depth
                int(parts[1])  # cluster_id
            except ValueError:
                continue
            inst = parts[0]
            gname = " ".join(parts[2:-1])
            if not gname:
                continue
            if inst in inst_to_group and inst_to_group[inst] != gname:
                duplicate_insts.add(inst)
            inst_to_group[inst] = gname

    unique_names = sorted(set(inst_to_group.values()))
    leaf_groups = [
        {
            "group_name": name,
            "parent": "",
            "type": "",
            "depth": 0,
            "child_group_count": 0,
            "direct_insts": [],
        }
        for name in unique_names
    ]
    return leaf_groups, inst_to_group, duplicate_insts


def load_membership(path: Path):
    """
    Accept either rtlmp_instance_to_cluster.txt (flat) or rtlmp_cluster_membership.txt
    (hierarchy dump). Detection matches research load_groups_file().
    """
    head = ""
    with path.open("r", encoding="utf-8") as f:
        for _ in range(64):
            line = f.readline()
            if not line:
                break
            head += line
    if "RTLMP cluster hierarchy" in head:
        return parse_cluster_membership_hierarchy(path)
    return parse_instance_to_cluster_txt(path)


def parse_plan_csv(path: Path):
    if not path.exists():
        return {}
    plan = {}
    with path.open("r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if row.get("kind") != "cluster":
                continue
            plan[row["name"]] = {
                "lx": float(row["lx"]),
                "ly": float(row["ly"]),
                "ux": float(row["ux"]),
                "uy": float(row["uy"]),
                "cx_um": float(row["cx_um"]),
                "cy_um": float(row["cy_um"]),
                "w_um": float(row["w_um"]),
                "h_um": float(row["h_um"]),
            }
    return plan


def tcl_quote(path: Path):
    return str(path).replace("\\", "\\\\").replace('"', '\\"')


def build_extract_tcl(odb_path: Path, placed_path: Path, core_path: Path):
    odb_q = tcl_quote(odb_path)
    placed_q = tcl_quote(placed_path)
    core_q = tcl_quote(core_path)
    return f"""read_db "{odb_q}"
set block [ord::get_db_block]
set tech [ord::get_db_tech]
set dbu [$tech getDbUnitsPerMicron]
set core [$block getCoreArea]

set fcore [open "{core_q}" w]
puts $fcore "dbu\\tcore_lx\\tcore_ly\\tcore_ux\\tcore_uy"
puts $fcore "$dbu\\t[$core xMin]\\t[$core yMin]\\t[$core xMax]\\t[$core yMax]"
close $fcore

set fout [open "{placed_q}" w]
puts $fout "inst\\tlx\\tly\\tux\\tuy\\tcx\\tcy\\tstatus"
foreach inst [$block getInsts] {{
  if {{ [$inst isBlock] }} {{
    continue
  }}
  if {{ ![$inst isPlaced] }} {{
    continue
  }}
  set box [$inst getBBox]
  set lx [$box xMin]
  set ly [$box yMin]
  set ux [$box xMax]
  set uy [$box yMax]
  set cx [expr {{($lx + $ux) / 2.0}}]
  set cy [expr {{($ly + $uy) / 2.0}}]
  puts $fout "[$inst getName]\\t$lx\\t$ly\\t$ux\\t$uy\\t$cx\\t$cy\\t[$inst getPlacementStatus]"
}}
close $fout
exit
"""


def build_env(openroad_bin: Path):
    env = os.environ.copy()
    env.pop("CONDA_PREFIX", None)
    env.pop("CONDA_DEFAULT_ENV", None)
    ld_library_path = env.get("LD_LIBRARY_PATH", "")
    kept = [
        entry
        for entry in ld_library_path.split(":")
        if entry and "conda" not in entry.lower() and "miniconda" not in entry.lower()
    ]
    if kept:
        env["LD_LIBRARY_PATH"] = ":".join(kept)
    else:
        env.pop("LD_LIBRARY_PATH", None)
    system_libstdcpp = Path("/usr/lib/x86_64-linux-gnu/libstdc++.so.6")
    if system_libstdcpp.exists():
        env["LD_PRELOAD"] = str(system_libstdcpp)
    env["PATH"] = f"{openroad_bin.parent}:{env.get('PATH', '')}"
    return env


def extract_placed_rows(openroad_bin: Path, odb_path: Path):
    with tempfile.TemporaryDirectory(prefix="placepng_") as tmpdir:
        tmp = Path(tmpdir)
        tcl_path = tmp / "dump.tcl"
        placed_path = tmp / "placed.tsv"
        core_path = tmp / "core.tsv"
        tcl_path.write_text(
            build_extract_tcl(odb_path, placed_path, core_path),
            encoding="utf-8",
        )
        try:
            subprocess.run(
                [str(openroad_bin), str(tcl_path)],
                check=True,
                env=build_env(openroad_bin),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                "OpenROAD extraction failed.\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            ) from exc

        with core_path.open("r", encoding="utf-8", newline="") as f:
            core_row = next(csv.DictReader(f, delimiter="\t"))
        with placed_path.open("r", encoding="utf-8", newline="") as f:
            placed_rows = list(csv.DictReader(f, delimiter="\t"))

    core = {
        "dbu": float(core_row["dbu"]),
        "lx": float(core_row["core_lx"]) / float(core_row["dbu"]),
        "ly": float(core_row["core_ly"]) / float(core_row["dbu"]),
        "ux": float(core_row["core_ux"]) / float(core_row["dbu"]),
        "uy": float(core_row["core_uy"]) / float(core_row["dbu"]),
    }

    rows = []
    for row in placed_rows:
        dbu = core["dbu"]
        rows.append(
            {
                "inst": row["inst"],
                "lx": float(row["lx"]) / dbu,
                "ly": float(row["ly"]) / dbu,
                "ux": float(row["ux"]) / dbu,
                "uy": float(row["uy"]) / dbu,
                "cx": float(row["cx"]) / dbu,
                "cy": float(row["cy"]) / dbu,
                "status": row["status"],
            }
        )
    return core, rows


def slugify(name: str):
    short = name.split("||")[0].split("/")[-1]
    return re.sub(r"[^A-Za-z0-9._-]+", "_", short).strip("._") or "cluster"


def color_map(group_names):
    cmap = plt.get_cmap("tab20", max(1, len(group_names)))
    return {name: cmap(idx) for idx, name in enumerate(group_names)}


def validate_inputs(
    odb_path: Path,
    membership_path: Path,
    plan_csv: Path | None,
    openroad_bin: Path,
):
    if not odb_path.exists():
        fail(f"--odb not found: {odb_path}")
    if not membership_path.exists():
        fail(f"--membership not found: {membership_path}")
    if plan_csv is not None and not plan_csv.exists():
        fail(f"--plan-csv not found: {plan_csv}")
    if not openroad_bin.exists():
        fail(f"--openroad not found: {openroad_bin}")


def analyze_consistency(grouped_rows, plan, matched: int, unmatched: int):
    grouped_names = set(grouped_rows.keys())
    plan_names = set(plan.keys())
    overlap = grouped_names & plan_names
    plan_only = plan_names - grouped_names
    grouped_only = grouped_names - plan_names
    total_cells = matched + unmatched
    unmatched_ratio = (unmatched / total_cells) if total_cells else 0.0

    return {
        "grouped_names": grouped_names,
        "plan_names": plan_names,
        "overlap": overlap,
        "plan_only": plan_only,
        "grouped_only": grouped_only,
        "total_cells": total_cells,
        "unmatched_ratio": unmatched_ratio,
    }


def draw_core(ax, core):
    ax.add_patch(
        patches.Rectangle(
            (core["lx"], core["ly"]),
            core["ux"] - core["lx"],
            core["uy"] - core["ly"],
            linewidth=1.8,
            edgecolor="black",
            facecolor="none",
        )
    )
    ax.set_xlim(core["lx"], core["ux"])
    ax.set_ylim(core["ly"], core["uy"])
    ax.set_aspect("equal")


def render_overview(out_path: Path, core, grouped_rows, plan, colors):
    fig, ax = plt.subplots(figsize=(12, 12))
    draw_core(ax, core)
    ax.set_title("Placed Standard Cells by RTLMP Soft Macro")

    for group_name, rect in plan.items():
        ax.add_patch(
            patches.Rectangle(
                (rect["lx"], rect["ly"]),
                rect["ux"] - rect["lx"],
                rect["uy"] - rect["ly"],
                linewidth=0.7,
                edgecolor=colors.get(group_name, "gray"),
                facecolor="none",
                alpha=0.35,
                linestyle="--",
            )
        )

    top_groups = sorted(grouped_rows.items(), key=lambda item: len(item[1]), reverse=True)
    for idx, (group_name, rows) in enumerate(top_groups):
        xs = [row["cx"] for row in rows]
        ys = [row["cy"] for row in rows]
        label = slugify(group_name) if idx < 12 else None
        ax.scatter(
            xs,
            ys,
            s=0.25,
            c=[colors[group_name]],
            alpha=0.55,
            linewidths=0,
            label=label,
            rasterized=True,
        )

    if top_groups:
        ax.legend(loc="upper right", fontsize=6, markerscale=8)
    plt.tight_layout()
    plt.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def render_individual(out_dir: Path, core, grouped_rows, plan, colors):
    out_dir.mkdir(parents=True, exist_ok=True)
    for group_name, rows in sorted(grouped_rows.items()):
        fig, ax = plt.subplots(figsize=(8, 8))
        draw_core(ax, core)
        ax.set_title(f"Placed Cells: {slugify(group_name)}")
        rect = plan.get(group_name)
        if rect is not None:
            ax.add_patch(
                patches.Rectangle(
                    (rect["lx"], rect["ly"]),
                    rect["ux"] - rect["lx"],
                    rect["uy"] - rect["ly"],
                    linewidth=1.6,
                    edgecolor=colors[group_name],
                    facecolor=colors[group_name],
                    alpha=0.14,
                )
            )
        xs = [row["cx"] for row in rows]
        ys = [row["cy"] for row in rows]
        ax.scatter(
            xs,
            ys,
            s=0.5,
            c=[colors[group_name]],
            alpha=0.75,
            linewidths=0,
            rasterized=True,
        )
        plt.tight_layout()
        plt.savefig(out_dir / f"{slugify(group_name)}.png", dpi=250, bbox_inches="tight")
        plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--odb", required=True)
    ap.add_argument(
        "--membership",
        default=str(DEFAULT_MEMBERSHIP),
        help=(
            "rtlmp_instance_to_cluster.txt (default: flow/reports/.../base/) "
            "or rtlmp_cluster_membership.txt hierarchy dump"
        ),
    )
    ap.add_argument("--out", required=True)
    ap.add_argument("--out-dir")
    ap.add_argument("--plan-csv")
    ap.add_argument(
        "--openroad",
        default="/home/piohuang/OpenROAD-flow-scripts/tools/install/OpenROAD/bin/openroad",
    )
    ap.add_argument(
        "--max-unmatched-ratio",
        type=float,
        default=0.95,
        help="Fail if unmatched_placed_cells / total_placed_cells exceeds this value.",
    )
    ap.add_argument(
        "--strict",
        action="store_true",
        help="Treat major consistency warnings as hard errors.",
    )
    args = ap.parse_args()

    odb_path = Path(args.odb).resolve()
    membership_path = Path(args.membership).resolve()
    out_path = Path(args.out).resolve()
    out_dir = (
        Path(args.out_dir).resolve()
        if args.out_dir
        else out_path.with_suffix("")
    )
    plan_csv = Path(args.plan_csv).resolve() if args.plan_csv else None
    openroad_bin = Path(args.openroad).resolve()

    validate_inputs(odb_path, membership_path, plan_csv, openroad_bin)
    leaf_groups, inst_to_group, duplicate_insts = load_membership(membership_path)
    if not leaf_groups:
        fail(
            "No cluster groups in membership input "
            "(empty rtlmp_instance_to_cluster.txt or no leaf groups in hierarchy dump)."
        )
    plan = parse_plan_csv(plan_csv) if plan_csv else {}
    core, placed_rows = extract_placed_rows(openroad_bin, odb_path)
    if not placed_rows:
        fail("No placed standard cells were extracted from ODB.")

    grouped_rows = {group["group_name"]: [] for group in leaf_groups}
    unmatched = 0
    for row in placed_rows:
        group_name = inst_to_group.get(row["inst"])
        if group_name is None:
            unmatched += 1
            continue
        grouped_rows.setdefault(group_name, []).append(row)

    grouped_rows = {name: rows for name, rows in grouped_rows.items() if rows}
    if not grouped_rows:
        fail("No placed cells matched membership groups. Inputs are likely inconsistent.")
    colors = color_map(sorted(grouped_rows))

    matched = sum(len(rows) for rows in grouped_rows.values())
    stats = analyze_consistency(grouped_rows, plan, matched, unmatched)
    if stats["unmatched_ratio"] > args.max_unmatched_ratio:
        fail(
            "Too many unmatched placed cells. "
            f"ratio={stats['unmatched_ratio']:.3f} "
            f"(threshold={args.max_unmatched_ratio:.3f}). "
            "Use matching ODB and membership files from the same run."
        )
    if plan and not stats["overlap"]:
        fail(
            "Plan CSV has zero name overlap with matched groups. "
            "Use plan/membership from the same RTLMP run."
        )
    if args.strict and plan and stats["grouped_only"]:
        fail(
            "Some matched groups are missing in plan CSV under --strict. "
            f"missing={len(stats['grouped_only'])}"
        )

    out_path.parent.mkdir(parents=True, exist_ok=True)
    render_overview(out_path, core, grouped_rows, plan, colors)
    render_individual(out_dir, core, grouped_rows, plan, colors)

    print(f"overview_png={out_path}")
    print(f"per_cluster_dir={out_dir}")
    print(f"leaf_groups={len(grouped_rows)}")
    print(f"matched_cells={matched}")
    print(f"unmatched_placed_cells={unmatched}")
    print(f"duplicate_membership_insts={len(duplicate_insts)}")
    print(f"unmatched_ratio={stats['unmatched_ratio']:.6f}")
    if plan:
        print(f"plan_cluster_count={len(stats['plan_names'])}")
        print(f"plan_overlap_count={len(stats['overlap'])}")
        print(f"plan_only_clusters={len(stats['plan_only'])}")
        print(f"grouped_without_plan={len(stats['grouped_only'])}")


if __name__ == "__main__":
    main()
