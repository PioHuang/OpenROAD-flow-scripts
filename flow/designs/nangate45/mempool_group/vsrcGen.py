#!/usr/bin/env python3
"""
Generate OpenROAD PSM vsrc locations from top-level VDD pin boxes.

Input format (TSV), expected from export_psm_vsrc.tcl:
  llx_um  lly_um  urx_um  ury_um  voltage_V  layer  bterm

Output format (CSV-like text), accepted by analyze_power_grid -vsrc:
  x_um, y_um, size_um, voltage_V

Optional: write an SVG (and/or PNG if matplotlib is installed) showing die/core
and pin rectangles plus generated vsrc centers.
"""

from __future__ import annotations

import argparse
import csv
import random
from pathlib import Path
from typing import Iterable


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate vsrc.loc from top-level power pin boxes TSV."
    )
    parser.add_argument(
        "--boxes",
        type=Path,
        default=Path(
            "../../../../reports/nangate45/mempool_group/base/psm_vsrc_boxes_VDD.tsv"
        ),
        help="Input psm_vsrc_boxes_<NET>.tsv path.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("vsrc.loc"),
        help="Output vsrc.loc path.",
    )
    parser.add_argument(
        "--bterm",
        default="VDD",
        help="Only include rows with this top-level bterm name (default: VDD).",
    )
    parser.add_argument(
        "--voltage",
        type=float,
        default=None,
        help="Override voltage for all generated points. Default uses TSV voltage.",
    )
    parser.add_argument(
        "--size-mode",
        choices=("max", "min", "fixed"),
        default="max",
        help="How to derive size_um from each box (default: max).",
    )
    parser.add_argument(
        "--fixed-size",
        type=float,
        default=15.0,
        help="size_um when --size-mode=fixed (default: 15.0).",
    )
    parser.add_argument(
        "--min-size",
        type=float,
        default=0.05,
        help="Minimum generated size_um floor (default: 0.05).",
    )
    parser.add_argument(
        "--scatter-pitch",
        type=float,
        default=0.0,
        help=(
            "Generate multiple sources per pin box on this pitch (µm). "
            "If <= 0, keep one source at box center."
        ),
    )
    parser.add_argument(
        "--scatter-margin",
        type=float,
        default=0.0,
        help="Inset margin (µm) from each pin-box edge when scattering points.",
    )
    parser.add_argument(
        "--scatter-jitter",
        type=float,
        default=0.0,
        help="Optional random jitter (µm) added to each scattered point.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=1234,
        help="Random seed for --scatter-jitter (default: 1234).",
    )
    parser.add_argument(
        "--layout",
        type=Path,
        default=None,
        help="Optional db_layout.tsv (die/core rects) for visualization framing.",
    )
    parser.add_argument(
        "--viz-svg",
        type=Path,
        default=None,
        help="Write chip visualization as SVG (no extra dependencies).",
    )
    parser.add_argument(
        "--viz-png",
        type=Path,
        default=None,
        help="Write chip visualization as PNG (requires matplotlib).",
    )
    parser.add_argument(
        "--viz-marker-cap-um",
        type=float,
        default=30.0,
        help="Cap drawn marker radius (µm) so huge stripe sizes stay readable.",
    )
    return parser.parse_args()


def load_layout(path: Path | None) -> tuple[tuple[float, float, float, float] | None, tuple[float, float, float, float] | None]:
    """Return (die_rect, core_rect) as (llx,lly,urx,ury) in µm, or (None, None)."""
    if path is None or not path.exists():
        return None, None
    die = None
    core = None
    with path.open("r", encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            kind = row.get("kind", "").strip()
            llx = float(row["llx_um"])
            lly = float(row["lly_um"])
            urx = float(row["urx_um"])
            ury = float(row["ury_um"])
            if kind == "die":
                die = (llx, lly, urx, ury)
            elif kind == "core":
                core = (llx, lly, urx, ury)
    return die, core


def bbox_from_boxes(boxes: Iterable[tuple[float, float, float, float]]) -> tuple[float, float, float, float]:
    xs_lo: list[float] = []
    ys_lo: list[float] = []
    xs_hi: list[float] = []
    ys_hi: list[float] = []
    for llx, lly, urx, ury in boxes:
        xs_lo.append(min(llx, urx))
        ys_lo.append(min(lly, ury))
        xs_hi.append(max(llx, urx))
        ys_hi.append(max(lly, ury))
    margin = 20.0
    return (
        min(xs_lo) - margin,
        min(ys_lo) - margin,
        max(xs_hi) + margin,
        max(ys_hi) + margin,
    )


def sample_axis_points(lo: float, hi: float, pitch: float) -> list[float]:
    if hi <= lo:
        return [0.5 * (lo + hi)]
    if pitch <= 0:
        return [0.5 * (lo + hi)]
    pts: list[float] = []
    x = lo + 0.5 * pitch
    while x < hi:
        pts.append(x)
        x += pitch
    if not pts:
        pts.append(0.5 * (lo + hi))
    return pts


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def write_viz_svg(
    path: Path,
    view: tuple[float, float, float, float],
    die: tuple[float, float, float, float] | None,
    core: tuple[float, float, float, float] | None,
    pin_boxes: list[tuple[float, float, float, float]],
    vsrc_points: list[tuple[float, float, float, float]],
    marker_cap_um: float,
) -> None:
    llx, lly, urx, ury = view
    w = max(urx - llx, 1e-6)
    h = max(ury - lly, 1e-6)
    # SVG y grows downward; chip coords y up — flip with transform
    def sx(x: float) -> float:
        return 1000.0 * (x - llx) / w

    def sy(y: float) -> float:
        return 1000.0 * (ury - y) / h

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000" '
            f'viewBox="0 0 1000 1000">\n'
        )
        f.write(
            '<rect x="0" y="0" width="1000" height="1000" fill="#0b1020"/>\n'
            '<g stroke-width="1.2">\n'
        )

        if die is not None:
            dlx, dly, drx, dry = die
            f.write(
                f'<rect x="{sx(dlx):.2f}" y="{sy(dry):.2f}" '
                f'width="{sx(drx) - sx(dlx):.2f}" height="{sy(dly) - sy(dry):.2f}" '
                'fill="none" stroke="#6b7a99" stroke-dasharray="8 6"/>\n'
            )
        if core is not None:
            clx, cly, crx, cry = core
            f.write(
                f'<rect x="{sx(clx):.2f}" y="{sy(cry):.2f}" '
                f'width="{sx(crx) - sx(clx):.2f}" height="{sy(cly) - sy(cry):.2f}" '
                'fill="none" stroke="#9aa7c7" stroke-dasharray="4 4"/>\n'
            )

        for bx in pin_boxes:
            plx, ply, prx, pry = bx
            f.write(
                f'<rect x="{sx(plx):.2f}" y="{sy(pry):.2f}" '
                f'width="{sx(prx) - sx(plx):.2f}" height="{sy(ply) - sy(pry):.2f}" '
                'fill="#ff980033" stroke="#ffb74d" stroke-width="1"/>\n'
            )

        for cx, cy, size, _volt in vsrc_points:
            r_draw = min(max(size / 2.0, 2.0), marker_cap_um)
            rx = 1000.0 * r_draw / w
            ry = 1000.0 * r_draw / h
            f.write(
                f'<ellipse cx="{sx(cx):.2f}" cy="{sy(cy):.2f}" rx="{rx:.2f}" ry="{ry:.2f}" '
                'fill="#26c6da33" stroke="#26c6da" stroke-width="1.5"/>\n'
            )
            cr = 4.0
            f.write(
                f'<line x1="{sx(cx) - cr:.2f}" y1="{sy(cy):.2f}" x2="{sx(cx) + cr:.2f}" y2="{sy(cy):.2f}" stroke="#eceff1" stroke-width="1.5"/>\n'
            )
            f.write(
                f'<line x1="{sx(cx):.2f}" y1="{sy(cy) - cr:.2f}" x2="{sx(cx):.2f}" y2="{sy(cy) + cr:.2f}" stroke="#eceff1" stroke-width="1.5"/>\n'
            )

        f.write("</g>\n")
        f.write(
            '<text x="20" y="36" fill="#eceff1" font-size="22" font-family="sans-serif">'
            "vsrcGen: VDD pin boxes (orange) + vsrc centers (cyan)</text>\n"
        )
        f.write("</svg>\n")


def write_viz_png(
    path: Path,
    view: tuple[float, float, float, float],
    die: tuple[float, float, float, float] | None,
    core: tuple[float, float, float, float] | None,
    pin_boxes: list[tuple[float, float, float, float]],
    vsrc_points: list[tuple[float, float, float, float]],
    marker_cap_um: float,
) -> None:
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle

    llx, lly, urx, ury = view
    fig, ax = plt.subplots(figsize=(10, 10))
    ax.set_facecolor("#0b1020")
    ax.set_xlim(llx, urx)
    ax.set_ylim(lly, ury)
    ax.set_aspect("equal")
    ax.invert_yaxis()

    if die is not None:
        dlx, dly, drx, dry = die
        ax.add_patch(
            Rectangle(
                (dlx, dly),
                drx - dlx,
                dry - dly,
                fill=False,
                edgecolor="#6b7a99",
                linewidth=1.2,
                linestyle=(0, (6, 6)),
            )
        )
    if core is not None:
        clx, cly, crx, cry = core
        ax.add_patch(
            Rectangle(
                (clx, cly),
                crx - clx,
                cry - cly,
                fill=False,
                edgecolor="#9aa7c7",
                linewidth=1.0,
                linestyle=(0, (3, 4)),
            )
        )

    for plx, ply, prx, pry in pin_boxes:
        ax.add_patch(
            Rectangle(
                (min(plx, prx), min(ply, pry)),
                abs(prx - plx),
                abs(pry - ply),
                facecolor=(1.0, 0.6, 0.0, 0.18),
                edgecolor="#ffb74d",
                linewidth=0.8,
            )
        )

    for cx, cy, size, _volt in vsrc_points:
        r_draw = min(max(size / 2.0, 2.0), marker_cap_um)
        circ = plt.Circle((cx, cy), r_draw, color="#26c6da", alpha=0.35, ec="#26c6da", linewidth=1.2)
        ax.add_patch(circ)
        ax.plot([cx - 3, cx + 3], [cy, cy], color="#eceff1", linewidth=1.2)
        ax.plot([cx, cx], [cy - 3, cy + 3], color="#eceff1", linewidth=1.2)

    ax.set_title("vsrcGen: VDD pin boxes + vsrc centers", color="#eceff1", fontsize=14)
    ax.tick_params(colors="#b0bec5")
    for spine in ax.spines.values():
        spine.set_color("#37474f")
    fig.patch.set_facecolor("#0b1020")
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    random.seed(args.seed)

    if args.size_mode == "fixed" and args.fixed_size <= 0:
        raise SystemExit("--fixed-size must be > 0 when --size-mode=fixed")

    if not args.boxes.exists():
        raise SystemExit(f"Input file not found: {args.boxes}")

    pin_boxes: list[tuple[float, float, float, float]] = []
    rows_out: list[tuple[float, float, float, float]] = []

    with args.boxes.open("r", encoding="utf-8") as f:
        reader = csv.reader(f, delimiter="\t")
        for row in reader:
            if not row or row[0].startswith("#"):
                continue
            if len(row) < 7:
                continue

            llx = float(row[0].strip())
            lly = float(row[1].strip())
            urx = float(row[2].strip())
            ury = float(row[3].strip())
            voltage = float(row[4].strip()) if args.voltage is None else args.voltage
            bterm = row[6].strip()

            if bterm != args.bterm:
                continue

            pin_boxes.append((llx, lly, urx, ury))

            x0 = min(llx, urx)
            x1 = max(llx, urx)
            y0 = min(lly, ury)
            y1 = max(lly, ury)
            dx = x1 - x0
            dy = y1 - y0

            if args.size_mode == "fixed":
                size = args.fixed_size
            elif args.size_mode == "min":
                size = min(dx, dy)
            else:
                size = max(dx, dy)

            size = max(size, args.min_size)

            if args.scatter_pitch > 0:
                sx0 = x0 + args.scatter_margin
                sx1 = x1 - args.scatter_margin
                sy0 = y0 + args.scatter_margin
                sy1 = y1 - args.scatter_margin
                if sx1 <= sx0:
                    sx0, sx1 = x0, x1
                if sy1 <= sy0:
                    sy0, sy1 = y0, y1

                xs = sample_axis_points(sx0, sx1, args.scatter_pitch)
                ys = sample_axis_points(sy0, sy1, args.scatter_pitch)
                for cx in xs:
                    for cy in ys:
                        if args.scatter_jitter > 0:
                            cx += random.uniform(-args.scatter_jitter, args.scatter_jitter)
                            cy += random.uniform(-args.scatter_jitter, args.scatter_jitter)
                            cx = clamp(cx, sx0, sx1)
                            cy = clamp(cy, sy0, sy1)
                        rows_out.append((cx, cy, size, voltage))
            else:
                cx = (x0 + x1) / 2.0
                cy = (y0 + y1) / 2.0
                rows_out.append((cx, cy, size, voltage))

    if not rows_out:
        raise SystemExit(
            f"No rows matched bterm={args.bterm!r} in input file: {args.boxes}"
        )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8") as f:
        for cx, cy, size, voltage in rows_out:
            f.write(f"{cx:.10g}, {cy:.10g}, {size:.10g}, {voltage:.10g}\n")

    print(f"Wrote {len(rows_out)} vsrc points to {args.out}")

    if args.viz_svg is not None or args.viz_png is not None:
        die, core = load_layout(args.layout)
        view = die if die is not None else bbox_from_boxes(pin_boxes)

        if args.viz_svg is not None:
            write_viz_svg(
                args.viz_svg,
                view,
                die,
                core,
                pin_boxes,
                rows_out,
                args.viz_marker_cap_um,
            )
            print(f"Wrote SVG visualization to {args.viz_svg}")

        if args.viz_png is not None:
            try:
                write_viz_png(
                    args.viz_png,
                    view,
                    die,
                    core,
                    pin_boxes,
                    rows_out,
                    args.viz_marker_cap_um,
                )
                print(f"Wrote PNG visualization to {args.viz_png}")
            except ImportError:
                print(
                    "matplotlib not installed; skip --viz-png. "
                    "Install matplotlib or use --viz-svg only."
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
