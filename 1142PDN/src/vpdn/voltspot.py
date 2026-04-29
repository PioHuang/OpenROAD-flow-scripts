import math
from typing import List, Tuple

from vpdn.model import Grid, Layer, Problem


def build_grid(problem: Problem) -> Grid:
    cw = problem.core.width_um()
    ch = problem.core.height_um()
    if cw <= 0.0 or ch <= 0.0:
        raise RuntimeError("Core size must be positive.")
    if problem.grid_pitch_um <= 0.0:
        raise RuntimeError("Grid pitch must be positive.")

    x_intervals = max(1, int(math.ceil(cw / problem.grid_pitch_um)))
    y_intervals = max(1, int(math.ceil(ch / problem.grid_pitch_um)))

    cols = x_intervals + 1
    rows = y_intervals + 1
    dx_um = cw / float(cols - 1)
    dy_um = ch / float(rows - 1)

    gx = 0.0
    gy = 0.0
    per_layer_edge_r: List[Tuple[str, float]] = []
    for layer in problem.layers:
        metal_cols = max(1, int(math.floor(cw / (2.0 * layer.pitch_um))))
        metal_rows = max(1, int(math.floor(ch / (2.0 * layer.pitch_um))))
        if layer.is_h:
            base_r = layer.r_ohm_per_um * cw / max(layer.width_um, 1e-9)
            edge_r = (1.0 / float(cols - 1)) * (float(rows) / float(metal_rows)) * base_r
            gx += 1.0 / edge_r
        else:
            base_r = layer.r_ohm_per_um * ch / max(layer.width_um, 1e-9)
            edge_r = (1.0 / float(rows - 1)) * (float(cols) / float(metal_cols)) * base_r
            gy += 1.0 / edge_r
        per_layer_edge_r.append((layer.name, edge_r))

    if gx <= 0.0 or gy <= 0.0:
        raise RuntimeError("Could not merge multi-layer conductance into virtual grid.")

    return Grid(
        rows=rows,
        cols=cols,
        dx_um=dx_um,
        dy_um=dy_um,
        rx_ohm=1.0 / gx,
        ry_ohm=1.0 / gy,
        per_layer_edge_r=per_layer_edge_r,
    )


def node_xy(problem: Problem, grid: Grid, row: int, col: int) -> Tuple[float, float]:
    x = problem.core.llx_um + col * grid.dx_um
    y = problem.core.lly_um + (grid.rows - 1 - row) * grid.dy_um
    return x, y


def node_id(grid: Grid, row: int, col: int) -> int:
    return row * grid.cols + col


def nearest_node(problem: Problem, grid: Grid, x_um: float, y_um: float) -> int:
    col = int(round((x_um - problem.core.llx_um) / grid.dx_um))
    row_from_bottom = int(round((y_um - problem.core.lly_um) / grid.dy_um))
    row = grid.rows - 1 - row_from_bottom
    row = min(max(row, 0), grid.rows - 1)
    col = min(max(col, 0), grid.cols - 1)
    return node_id(grid, row, col)


def rect_nodes(problem: Problem, grid: Grid, llx_um: float, lly_um: float, urx_um: float, ury_um: float) -> List[int]:
    hits = []
    for row in range(grid.rows):
        for col in range(grid.cols):
            idx = node_id(grid, row, col)
            x, y = node_xy(problem, grid, row, col)
            if llx_um <= x <= urx_um and lly_um <= y <= ury_um:
                hits.append(idx)
    return hits
