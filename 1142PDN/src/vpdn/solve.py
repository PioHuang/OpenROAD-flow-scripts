import math
from pathlib import Path
from typing import Dict, List, Tuple

from vpdn.model import Grid, Problem, SoftCluster, SolveResult
from vpdn.orfs import Knapsack
from vpdn.voltspot import nearest_node, node_id, node_xy, rect_nodes


def tile_rect(problem: Problem, grid: Grid, row: int, col: int):
    x, y = node_xy(problem, grid, row, col)
    if col == 0:
        llx = problem.core.llx_um
    else:
        px, _ = node_xy(problem, grid, row, col - 1)
        llx = 0.5 * (x + px)
    if col == grid.cols - 1:
        urx = problem.core.urx_um
    else:
        nx, _ = node_xy(problem, grid, row, col + 1)
        urx = 0.5 * (x + nx)
    if row == grid.rows - 1:
        lly = problem.core.lly_um
    else:
        _, py = node_xy(problem, grid, row + 1, col)
        lly = 0.5 * (y + py)
    if row == 0:
        ury = problem.core.ury_um
    else:
        _, ny = node_xy(problem, grid, row - 1, col)
        ury = 0.5 * (y + ny)
    return llx, lly, urx, ury


def overlap_area(a, b) -> float:
    llx = max(a[0], b[0])
    lly = max(a[1], b[1])
    urx = min(a[2], b[2])
    ury = min(a[3], b[3])
    return max(0.0, urx - llx) * max(0.0, ury - lly)


def assign_loads(problem: Problem, grid: Grid):
    n = grid.rows * grid.cols
    load_a = [0.0] * n
    hard_map = []
    soft_map = []

    for hard in problem.hard:
        idx = nearest_node(problem, grid, hard.x_um, hard.y_um)
        load_a[idx] += hard.current_a
        hard_map.append((hard.name, idx, hard.x_um, hard.y_um, hard.current_a))

    for soft in problem.soft:
        kn = Knapsack(soft.items)
        rect = (soft.rect.llx_um, soft.rect.lly_um, soft.rect.urx_um, soft.rect.ury_um)
        soft.mesh_sum_a = 0.0
        for row in range(grid.rows):
            for col in range(grid.cols):
                idx = node_id(grid, row, col)
                area = overlap_area(tile_rect(problem, grid, row, col), rect)
                if area <= 0.0:
                    continue
                cur = kn.query(area)
                if cur <= 0.0:
                    continue
                load_a[idx] += cur
                soft.mesh_sum_a += cur
                soft_map.append((soft.name, idx, cur))

    return load_a, hard_map, soft_map


def assign_sources(problem: Problem, grid: Grid):
    n = grid.rows * grid.cols
    src_g = [0.0] * n
    selected = set()
    for src in problem.sources:
        hits = rect_nodes(problem, grid, src.rect.llx_um, src.rect.lly_um, src.rect.urx_um, src.rect.ury_um)
        if not hits:
            cx, cy = src.rect.center()
            hits = [nearest_node(problem, grid, cx, cy)]
        for idx in hits:
            selected.add(idx)
    g_src = 1.0 / max(problem.pad_r_ohm, 1e-12)
    for idx in selected:
        src_g[idx] = g_src
    return src_g


def matvec(grid: Grid, src_g: List[float], x: List[float]) -> List[float]:
    out = [0.0] * len(x)
    gx = 1.0 / max(grid.rx_ohm, 1e-18)
    gy = 1.0 / max(grid.ry_ohm, 1e-18)
    for row in range(grid.rows):
        for col in range(grid.cols):
            idx = node_id(grid, row, col)
            val = src_g[idx] * x[idx]
            if col > 0:
                j = node_id(grid, row, col - 1)
                val += gx * (x[idx] - x[j])
            if col + 1 < grid.cols:
                j = node_id(grid, row, col + 1)
                val += gx * (x[idx] - x[j])
            if row > 0:
                j = node_id(grid, row - 1, col)
                val += gy * (x[idx] - x[j])
            if row + 1 < grid.rows:
                j = node_id(grid, row + 1, col)
                val += gy * (x[idx] - x[j])
            out[idx] = val
    return out


def diag(grid: Grid, src_g: List[float]) -> List[float]:
    gx = 1.0 / max(grid.rx_ohm, 1e-18)
    gy = 1.0 / max(grid.ry_ohm, 1e-18)
    out = [0.0] * (grid.rows * grid.cols)
    for row in range(grid.rows):
        for col in range(grid.cols):
            idx = node_id(grid, row, col)
            d = src_g[idx]
            if col > 0:
                d += gx
            if col + 1 < grid.cols:
                d += gx
            if row > 0:
                d += gy
            if row + 1 < grid.rows:
                d += gy
            out[idx] = d
    return out


def dot(a: List[float], b: List[float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def solve(problem: Problem, grid: Grid, max_iter: int = 5000, rel_tol: float = 1e-8) -> SolveResult:
    load_a, hard_map, soft_map = assign_loads(problem, grid)
    src_g = assign_sources(problem, grid)
    n = grid.rows * grid.cols
    rhs = [src_g[i] * problem.vdd - load_a[i] for i in range(n)]
    d = diag(grid, src_g)
    x = [problem.vdd] * n
    ax = matvec(grid, src_g, x)
    r = [rhs[i] - ax[i] for i in range(n)]
    z = [r[i] / max(d[i], 1e-18) for i in range(n)]
    p = z[:]
    rz = dot(r, z)
    rhs_norm = math.sqrt(max(dot(rhs, rhs), 1e-30))
    iters = 0
    for iters in range(1, max_iter + 1):
        ap = matvec(grid, src_g, p)
        den = dot(p, ap)
        if abs(den) <= 1e-30:
            break
        alpha = rz / den
        for i in range(n):
            x[i] += alpha * p[i]
            r[i] -= alpha * ap[i]
        resid = math.sqrt(max(dot(r, r), 0.0))
        if resid <= rel_tol * rhs_norm:
            break
        z = [r[i] / max(d[i], 1e-18) for i in range(n)]
        rz_new = dot(r, z)
        beta = rz_new / max(rz, 1e-30)
        rz = rz_new
        for i in range(n):
            p[i] = z[i] + beta * p[i]
    resid = math.sqrt(max(dot(r, r), 0.0))
    return SolveResult(
        volt_v=x,
        load_a=load_a,
        src_g=src_g,
        hard_map=hard_map,
        soft_map=soft_map,
        iters=iters,
        resid=resid,
    )


def build_edges(grid: Grid) -> List[Tuple[int, int, str, float]]:
    out = []
    for row in range(grid.rows):
        for col in range(grid.cols):
            idx = node_id(grid, row, col)
            if col + 1 < grid.cols:
                out.append((idx, node_id(grid, row, col + 1), "x", grid.rx_ohm))
            if row + 1 < grid.rows:
                out.append((idx, node_id(grid, row + 1, col), "y", grid.ry_ohm))
    return out


def dump(problem: Problem, grid: Grid, sol: SolveResult, dump_dir: Path):
    dump_dir.mkdir(parents=True, exist_ok=True)

    with open(dump_dir / "nodes.tsv", "w", encoding="utf-8") as f:
        f.write("id\trow\tcol\tx_um\ty_um\tvoltage_v\tdrop_pct\tload_a\tsrc_g\n")
        for row in range(grid.rows):
            for col in range(grid.cols):
                idx = node_id(grid, row, col)
                x, y = node_xy(problem, grid, row, col)
                v = sol.volt_v[idx]
                drop_pct = 100.0 * (problem.vdd - v) / max(problem.vdd, 1e-12)
                f.write(
                    f"{idx}\t{row}\t{col}\t{x:.6f}\t{y:.6f}\t{v:.9f}\t{drop_pct:.9f}\t"
                    f"{sol.load_a[idx]:.9e}\t{sol.src_g[idx]:.9e}\n"
                )

    with open(dump_dir / "edges.tsv", "w", encoding="utf-8") as f:
        f.write("u\tv\tkind\tres_ohm\n")
        for u, v, kind, r in build_edges(grid):
            f.write(f"{u}\t{v}\t{kind}\t{r:.9e}\n")

    with open(dump_dir / "hard.tsv", "w", encoding="utf-8") as f:
        f.write("name\tnode_id\tx_um\ty_um\tcurrent_a\n")
        for name, idx, x, y, cur in sol.hard_map:
            f.write(f"{name}\t{idx}\t{x:.6f}\t{y:.6f}\t{cur:.9e}\n")

    with open(dump_dir / "hardmacro.tsv", "w", encoding="utf-8") as f:
        f.write("name\tllx_um\tlly_um\turx_um\tury_um\tpin_count\ttotal_current_a\n")
        for m in problem.hard_macros:
            f.write(
                f"{m.name}\t{m.rect.llx_um:.6f}\t{m.rect.lly_um:.6f}\t{m.rect.urx_um:.6f}\t{m.rect.ury_um:.6f}\t"
                f"{m.pin_count}\t{m.total_current_a:.9e}\n"
            )

    with open(dump_dir / "soft.tsv", "w", encoding="utf-8") as f:
        f.write("cluster_id\tname\tllx_um\tlly_um\turx_um\tury_um\tobserved_a\tworst_a\tmesh_sum_a\tinstance_count\n")
        for soft in problem.soft:
            f.write(
                f"{soft.cluster_id}\t{soft.name}\t{soft.rect.llx_um:.6f}\t{soft.rect.lly_um:.6f}\t{soft.rect.urx_um:.6f}\t"
                f"{soft.rect.ury_um:.6f}\t{soft.observed_a:.9e}\t{soft.worst_a:.9e}\t{soft.mesh_sum_a:.9e}\t{soft.instance_count}\n"
            )

    with open(dump_dir / "sources.tsv", "w", encoding="utf-8") as f:
        f.write("layer\tllx_um\tlly_um\turx_um\tury_um\tvoltage_v\n")
        for src in problem.sources:
            f.write(
                f"{src.layer}\t{src.rect.llx_um:.6f}\t{src.rect.lly_um:.6f}\t{src.rect.urx_um:.6f}\t"
                f"{src.rect.ury_um:.6f}\t{src.voltage_v:.6f}\n"
            )

    with open(dump_dir / "layers.tsv", "w", encoding="utf-8") as f:
        f.write("name\trank\tdir\tpitch_um\twidth_um\tr_ohm_per_um\tvirt_edge_r_ohm\n")
        by_name = dict(grid.per_layer_edge_r)
        for layer in problem.layers:
            f.write(
                f"{layer.name}\t{layer.rank}\t{'H' if layer.is_h else 'V'}\t{layer.pitch_um:.6f}\t"
                f"{layer.width_um:.6f}\t{layer.r_ohm_per_um:.9e}\t{by_name[layer.name]:.9e}\n"
            )

    with open(dump_dir / "meta.tsv", "w", encoding="utf-8") as f:
        f.write("key\tvalue\n")
        vals = {
            "design": problem.name,
            "core_llx_um": problem.core.llx_um,
            "core_lly_um": problem.core.lly_um,
            "core_urx_um": problem.core.urx_um,
            "core_ury_um": problem.core.ury_um,
            "vdd": problem.vdd,
            "grid_pitch_um": problem.grid_pitch_um,
            "pad_r_ohm": problem.pad_r_ohm,
            "grid_intv": problem.grid_intv,
            "rows": grid.rows,
            "cols": grid.cols,
            "dx_um": grid.dx_um,
            "dy_um": grid.dy_um,
            "rx_ohm": grid.rx_ohm,
            "ry_ohm": grid.ry_ohm,
            "obs_layer": problem.obs_layer,
            "iters": sol.iters,
            "resid": sol.resid,
            "min_v": min(sol.volt_v),
            "max_v": max(sol.volt_v),
        }
        for k, v in vals.items():
            f.write(f"{k}\t{v}\n")

    with open(dump_dir / "gridvol.tsv", "w", encoding="utf-8") as f:
        f.write("x_idx\ty_idx\tdrop_pct\tvoltage_v\n")
        for row in range(grid.rows):
            for col in range(grid.cols):
                idx = node_id(grid, row, col)
                v = sol.volt_v[idx]
                drop_pct = 100.0 * (problem.vdd - v) / max(problem.vdd, 1e-12)
                x_idx = col
                y_idx = grid.rows - 1 - row
                f.write(f"{x_idx}\t{y_idx}\t{drop_pct:.9f}\t{v:.9f}\n")
