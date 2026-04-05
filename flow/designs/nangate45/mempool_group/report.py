#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patches as patches


def classify(name: str, width_dbu: int, height_dbu: int) -> str:
    if name.startswith("ios_"):
        return "io_boundary"
    if width_dbu <= 1 or height_dbu <= 1:
        return "point"
    if "sram_instance" in name or "fr_sp_instance" in name:
        return "hard_macro"
    return "cluster"


def short_name(name: str) -> str:
    chunk = name.split("||")[0]
    return chunk.split("/")[-1]


def read_fp(path: Path, dbu_per_micron: float):
    rows = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 5:
                raise ValueError(f"Unexpected RTLMP floorplan line: {line}")
            name, x_s, y_s, w_s, h_s = parts
            x = int(x_s)
            y = int(y_s)
            w = int(w_s)
            h = int(h_s)
            kind = classify(name, w, h)
            rows.append(
                {
                    "name": name,
                    "display_name": short_name(name),
                    "kind": kind,
                    "x_dbu": x,
                    "y_dbu": y,
                    "w_dbu": w,
                    "h_dbu": h,
                    "lx": x / dbu_per_micron,
                    "ly": y / dbu_per_micron,
                    "ux": (x + w) / dbu_per_micron,
                    "uy": (y + h) / dbu_per_micron,
                    "w_um": w / dbu_per_micron,
                    "h_um": h / dbu_per_micron,
                    "cx_um": (x + (w / 2.0)) / dbu_per_micron,
                    "cy_um": (y + (h / 2.0)) / dbu_per_micron,
                    "area_um2": (w * h) / (dbu_per_micron * dbu_per_micron),
                }
            )
    return rows


def write_csv(rows, out_csv: Path):
    fieldnames = [
        "name",
        "display_name",
        "kind",
        "x_dbu",
        "y_dbu",
        "w_dbu",
        "h_dbu",
        "lx",
        "ly",
        "ux",
        "uy",
        "w_um",
        "h_um",
        "cx_um",
        "cy_um",
        "area_um2",
    ]
    with out_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def render_png(rows, out_png: Path, core):
    fig, ax = plt.subplots(figsize=(12, 12))
    core_lx, core_ly, core_ux, core_uy = core
    ax.set_xlim(core_lx, core_ux)
    ax.set_ylim(core_ly, core_uy)
    ax.set_aspect("equal")
    ax.set_title("RTLMP Cluster Floorplan")

    ax.add_patch(
        patches.Rectangle(
            (core_lx, core_ly),
            core_ux - core_lx,
            core_uy - core_ly,
            linewidth=2,
            edgecolor="black",
            facecolor="none",
            label="Core",
        )
    )

    hard_macros = [r for r in rows if r["kind"] == "hard_macro"]
    clusters = [r for r in rows if r["kind"] == "cluster"]

    for idx, row in enumerate(hard_macros):
        label = "Hard macro" if idx == 0 else None
        ax.add_patch(
            patches.Rectangle(
                (row["lx"], row["ly"]),
                row["w_um"],
                row["h_um"],
                linewidth=1.2,
                edgecolor="darkred",
                facecolor="salmon",
                alpha=0.8,
                label=label,
            )
        )

    cmap = plt.get_cmap("tab20", max(1, len(clusters)))
    for idx, row in enumerate(clusters):
        color = cmap(idx)
        ax.add_patch(
            patches.Rectangle(
                (row["lx"], row["ly"]),
                row["w_um"],
                row["h_um"],
                linewidth=1.5,
                edgecolor=color,
                facecolor=color,
                alpha=0.28,
            )
        )
        if row["w_um"] > 50 and row["h_um"] > 35:
            ax.text(
                row["cx_um"],
                row["cy_um"],
                row["display_name"],
                ha="center",
                va="center",
                fontsize=6,
                bbox=dict(facecolor="white", alpha=0.55, edgecolor="none", pad=1),
            )

    ax.legend(loc="upper right")
    plt.tight_layout()
    plt.savefig(out_png, dpi=300, bbox_inches="tight")


def write_report(rows, out_report: Path):
    clusters = sorted(
        [r for r in rows if r["kind"] == "cluster"],
        key=lambda r: r["area_um2"],
        reverse=True,
    )
    hard_macros = sorted(
        [r for r in rows if r["kind"] == "hard_macro"],
        key=lambda r: r["area_um2"],
        reverse=True,
    )
    points = [r for r in rows if r["kind"] == "point"]
    ios = [r for r in rows if r["kind"] == "io_boundary"]

    total_cluster_area = sum(r["area_um2"] for r in clusters)
    total_macro_area = sum(r["area_um2"] for r in hard_macros)

    with out_report.open("w", encoding="utf-8") as f:
        f.write("# RTLMP Cluster Report\n\n")
        f.write("## Summary\n\n")
        f.write(f"- Total entries in `root.fp.txt`: {len(rows)}\n")
        f.write(f"- Cluster rectangles: {len(clusters)}\n")
        f.write(f"- Hard macro rectangles: {len(hard_macros)}\n")
        f.write(f"- Point-like entries (1x1 dbu): {len(points)}\n")
        f.write(f"- IO boundary entries: {len(ios)}\n")
        f.write(f"- Total cluster area: {total_cluster_area:.2f} um^2\n")
        f.write(f"- Total hard macro area: {total_macro_area:.2f} um^2\n\n")

        f.write("## Largest Clusters\n\n")
        f.write("| name | center (um) | size (um) | area (um^2) |\n")
        f.write("| --- | --- | --- | ---: |\n")
        for row in clusters[:10]:
            f.write(
                f"| `{row['name']}` | ({row['cx_um']:.1f}, {row['cy_um']:.1f}) | "
                f"{row['w_um']:.1f} x {row['h_um']:.1f} | {row['area_um2']:.1f} |\n"
            )

        f.write("\n## Largest Hard Macros\n\n")
        f.write("| name | center (um) | size (um) | area (um^2) |\n")
        f.write("| --- | --- | --- | ---: |\n")
        for row in hard_macros[:10]:
            f.write(
                f"| `{row['name']}` | ({row['cx_um']:.1f}, {row['cy_um']:.1f}) | "
                f"{row['w_um']:.1f} x {row['h_um']:.1f} | {row['area_um2']:.1f} |\n"
            )

        if points:
            f.write("\n## Notes\n\n")
            f.write(
                "- `point` entries are 1x1 dbu bookkeeping/anchor objects emitted by RTLMP; "
                "they are excluded from the PNG cluster rectangles.\n"
            )
            f.write(
                "- `hard_macro` entries are real macro leaf rectangles and are shown in red.\n"
            )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fp", required=True)
    ap.add_argument("--out-csv", required=True)
    ap.add_argument("--out-png", required=True)
    ap.add_argument("--out-report", required=True)
    ap.add_argument("--dbu-per-micron", type=float, default=2000.0)
    ap.add_argument("--core-lx", type=float, default=10.070)
    ap.add_argument("--core-ly", type=float, default=12.600)
    ap.add_argument("--core-ux", type=float, default=1089.840)
    ap.add_argument("--core-uy", type=float, default=1089.200)
    args = ap.parse_args()

    rows = read_fp(Path(args.fp), args.dbu_per_micron)
    write_csv(rows, Path(args.out_csv))
    render_png(
        rows,
        Path(args.out_png),
        (args.core_lx, args.core_ly, args.core_ux, args.core_uy),
    )
    write_report(rows, Path(args.out_report))


if __name__ == "__main__":
    main()
