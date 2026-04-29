from dataclasses import dataclass, field
from typing import List, Tuple


@dataclass
class Rect:
    llx_um: float
    lly_um: float
    urx_um: float
    ury_um: float

    def width_um(self) -> float:
        return max(0.0, self.urx_um - self.llx_um)

    def height_um(self) -> float:
        return max(0.0, self.ury_um - self.lly_um)

    def area_um2(self) -> float:
        return self.width_um() * self.height_um()

    def center(self) -> Tuple[float, float]:
        return ((self.llx_um + self.urx_um) * 0.5, (self.lly_um + self.ury_um) * 0.5)


@dataclass
class Layer:
    name: str
    rank: int
    is_h: bool
    pitch_um: float
    width_um: float
    r_ohm_per_um: float


@dataclass
class Source:
    layer: str
    rect: Rect
    voltage_v: float


@dataclass
class HardLoad:
    name: str
    x_um: float
    y_um: float
    current_a: float


@dataclass
class HardMacro:
    name: str
    rect: Rect
    pin_count: int
    total_current_a: float


@dataclass
class SoftCluster:
    name: str
    rect: Rect
    total_a: float
    observed_a: float = 0.0
    worst_a: float = 0.0
    mesh_sum_a: float = 0.0
    cluster_id: int = -1
    instance_count: int = 0
    items: List[Tuple[float, float]] = field(default_factory=list)


@dataclass
class Problem:
    name: str
    core: Rect
    layers: List[Layer]
    sources: List[Source]
    hard: List[HardLoad]
    hard_macros: List[HardMacro]
    soft: List[SoftCluster]
    vdd: float
    grid_pitch_um: float
    pad_r_ohm: float
    grid_intv: int
    obs_layer: str


@dataclass
class Grid:
    rows: int
    cols: int
    dx_um: float
    dy_um: float
    rx_ohm: float
    ry_ohm: float
    per_layer_edge_r: List[Tuple[str, float]]


@dataclass
class SolveResult:
    volt_v: List[float]
    load_a: List[float]
    src_g: List[float]
    hard_map: List[Tuple[str, int, float, float, float]]
    soft_map: List[Tuple[str, int, float]]
    iters: int
    resid: float
