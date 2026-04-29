#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path


def read_membership(path: Path):
    out = {}
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            if len(parts) < 3:
                continue
            inst = parts[0]
            # cluster_name may contain spaces; depth is the last token
            cname = " ".join(parts[2:-1]) if len(parts) > 3 else parts[2]
            out[inst] = cname
    return out


def read_cluster_boxes(path: Path):
    by_name = {}
    with path.open("r", encoding="utf-8") as f:
        r = csv.DictReader(f)
        for row in r:
            if row.get("kind") != "cluster":
                continue
            name = (row.get("name") or "").strip()
            disp = (row.get("display_name") or "").strip()
            if not name and not disp:
                continue
            box = (
                float(row["lx"]),
                float(row["ly"]),
                float(row["ux"]),
                float(row["uy"]),
            )
            if name:
                by_name[name] = box
            if disp:
                by_name[disp] = box
    return by_name


def main():
    ap = argparse.ArgumentParser(description="Build GPL soft-guidance file from RTLMP reports.")
    ap.add_argument("--membership", required=True, help="rtlmp_instance_to_cluster.txt")
    ap.add_argument("--clusters-csv", required=True, help="rtlmp_clusters.csv")
    ap.add_argument("--out", required=True, help="Output guidance file")
    args = ap.parse_args()

    membership = read_membership(Path(args.membership))
    boxes = read_cluster_boxes(Path(args.clusters_csv))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    kept = 0
    skipped = 0
    with out_path.open("w", encoding="utf-8") as f:
        f.write("instance_name\tcluster_name\ttarget_cx\ttarget_cy\ttarget_sigma_x\ttarget_sigma_y\n")
        for inst, cname in membership.items():
            box = boxes.get(cname)
            if box is None:
                skipped += 1
                continue
            llx, lly, urx, ury = box
            cx = 0.5 * (llx + urx)
            cy = 0.5 * (lly + ury)
            # gpl soft guidance uses Gaussian pull per cluster.
            # User-tuned setting: tighter pull so most mass stays within box.
            sx = max(0.1, (urx - llx) / 8.0)
            sy = max(0.1, (ury - lly) / 8.0)
            f.write(f"{inst}\t{cname}\t{cx:.6f}\t{cy:.6f}\t{sx:.6f}\t{sy:.6f}\n")
            kept += 1
    print(f"wrote {out_path} entries={kept} skipped={skipped}")


if __name__ == "__main__":
    main()
