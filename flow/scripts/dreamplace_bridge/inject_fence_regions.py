#!/usr/bin/env python3
"""
inject_fence_regions.py

Reads RTLMP cluster data and the exported DEF, then injects
DEF REGIONS (TYPE FENCE) and GROUPS sections so DREAMPlace
constrains standard cells within their RTLMP cluster boundaries.

Usage:
    python inject_fence_regions.py \
        --clusters-csv  <rtlmp_clusters.csv> \
        --instance-map  <rtlmp_instance_to_cluster.txt> \
        --def-input     <2_floorplan_for_dp.def> \
        --def-output    <2_floorplan_fenced.def>
"""

import argparse
import csv
import sys
from collections import defaultdict


def parse_clusters_csv(csv_path):
    """Return dict: cluster_name -> (lx_dbu, ly_dbu, ux_dbu, uy_dbu)
    Only includes entries where kind == 'cluster'."""
    clusters = {}
    with open(csv_path, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["kind"] != "cluster":
                continue
            name = row["name"]
            x_dbu = int(row["x_dbu"])
            y_dbu = int(row["y_dbu"])
            w_dbu = int(row["w_dbu"])
            h_dbu = int(row["h_dbu"])
            clusters[name] = (x_dbu, y_dbu, x_dbu + w_dbu, y_dbu + h_dbu)
    return clusters


def parse_instance_map(map_path, valid_clusters):
    """Return dict: cluster_name -> [instance_name, ...]
    Only includes instances belonging to clusters in valid_clusters."""
    cluster_instances = defaultdict(list)
    with open(map_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            inst_name = parts[0]
            # cluster_name is everything between cluster_id and depth
            cluster_name = " ".join(parts[2:-1])
            if cluster_name in valid_clusters:
                cluster_instances[cluster_name].append(inst_name)
    return cluster_instances


def sanitize_def_name(idx):
    """Generate a safe DEF identifier for a fence region."""
    return f"fence_region_{idx}"


def strip_def_sections(def_content, sections_to_strip):
    """Remove specified top-level DEF sections (e.g. SPECIALNETS, GROUPS, REGIONS).
    Each section is bounded by '<SECTION> <N> ;' and 'END <SECTION>'."""
    lines = def_content.split("\n")
    out_lines = []
    in_strip = False
    current_section = None
    for line in lines:
        stripped = line.strip()
        if not in_strip:
            for sec in sections_to_strip:
                if stripped.startswith(f"{sec} "):
                    in_strip = True
                    current_section = sec
                    break
            if not in_strip:
                out_lines.append(line)
        else:
            if stripped == f"END {current_section}":
                in_strip = False
                current_section = None
    return "\n".join(out_lines)


def inject_regions_and_groups(def_input, def_output, clusters, cluster_instances):
    """Read input DEF, inject REGIONS + GROUPS before END DESIGN, write output."""
    # Build ordered list of (cluster_name, bbox, safe_name)
    fence_entries = []
    for idx, (cname, bbox) in enumerate(sorted(clusters.items())):
        instances = cluster_instances.get(cname, [])
        if not instances:
            print(f"  WARNING: cluster '{cname}' has 0 mapped instances, skipping fence",
                  file=sys.stderr)
            continue
        safe_name = sanitize_def_name(idx)
        fence_entries.append((cname, bbox, safe_name, instances))

    print(f"  Injecting {len(fence_entries)} fence regions "
          f"({sum(len(e[3]) for e in fence_entries)} total instances)")

    # Build the REGIONS section
    regions_lines = [f"REGIONS {len(fence_entries)} ;"]
    for _cname, (lx, ly, ux, uy), safe_name, _insts in fence_entries:
        regions_lines.append(f"    - {safe_name} ( {lx} {ly} ) ( {ux} {uy} ) + TYPE FENCE ;")
    regions_lines.append("END REGIONS")

    # Build the GROUPS section
    groups_lines = [f"GROUPS {len(fence_entries)} ;"]
    for _cname, _bbox, safe_name, instances in fence_entries:
        # DEF GROUPS syntax: - group_name member1 member2 ... + REGION region_name ;
        # For large groups, write members on continuation lines
        groups_lines.append(f"    - {safe_name}")
        chunk_size = 20
        for i in range(0, len(instances), chunk_size):
            chunk = instances[i:i + chunk_size]
            groups_lines.append("      " + " ".join(chunk))
        groups_lines.append(f"    + REGION {safe_name} ;")
    groups_lines.append("END GROUPS")

    # Read input DEF, strip SPECIALNETS, inject before END DESIGN
    with open(def_input, "r") as f:
        def_content = f.read()

    def_content = strip_def_sections(def_content, ["SPECIALNETS", "GROUPS", "REGIONS"])
    print("  Stripped SPECIALNETS, existing GROUPS, and REGIONS sections")

    end_design_marker = "END DESIGN"
    insert_pos = def_content.rfind(end_design_marker)
    if insert_pos == -1:
        print("ERROR: 'END DESIGN' not found in input DEF", file=sys.stderr)
        sys.exit(1)

    injection = "\n".join(regions_lines) + "\n\n" + "\n".join(groups_lines) + "\n\n"
    new_def = def_content[:insert_pos] + injection + def_content[insert_pos:]

    with open(def_output, "w") as f:
        f.write(new_def)

    print(f"  Written: {def_output}")


def main():
    parser = argparse.ArgumentParser(
        description="Inject RTLMP cluster fence regions into DEF for DREAMPlace")
    parser.add_argument("--clusters-csv", required=True,
                        help="Path to rtlmp_clusters.csv")
    parser.add_argument("--instance-map", required=True,
                        help="Path to rtlmp_instance_to_cluster.txt")
    parser.add_argument("--def-input", required=True,
                        help="Input DEF (from OpenROAD export)")
    parser.add_argument("--def-output", required=True,
                        help="Output DEF with injected REGIONS/GROUPS")
    args = parser.parse_args()

    print("Step 1: Parsing RTLMP cluster boundaries...")
    clusters = parse_clusters_csv(args.clusters_csv)
    print(f"  Found {len(clusters)} cluster regions")

    print("Step 2: Parsing instance-to-cluster mapping...")
    cluster_instances = parse_instance_map(args.instance_map, clusters)
    total_mapped = sum(len(v) for v in cluster_instances.values())
    print(f"  Mapped {total_mapped} instances to {len(cluster_instances)} clusters")

    print("Step 3: Injecting fence regions into DEF...")
    inject_regions_and_groups(
        args.def_input, args.def_output, clusters, cluster_instances)

    print("Done.")


if __name__ == "__main__":
    main()
