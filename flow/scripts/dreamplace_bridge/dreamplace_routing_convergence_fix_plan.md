---
name: DREAMPlace Routing Convergence Fix
overview: 调整 DREAMPlace JSON 配置参数以改善 placement 的密度均匀性和可绕线性，使 OpenROAD global routing 能够收敛。
todos:
  - id: update-json
    content: 修改 mempool_group_fenced.json：更新 density_weight、gamma、num_bins、stop_overflow、ignore_net_degree、global_place_stages、routability_opt_flag 等参数
    status: completed
  - id: rerun-dreamplace
    content: 重新执行 DREAMPlace placement（run_dreamplace_flow.sh）
    status: in_progress
  - id: continue-openroad
    content: 接续 OpenROAD flow（3_2 IO placement -> skip 3_3 -> 3_4/3_5 -> CTS -> route -> finish）
    status: pending
isProject: false
---

# DREAMPlace 参数调整方案 — 解决 Routing 不收敛

## 问题诊断

从目前的 log 和 placement 图像可以观察到以下问题：

1. **DREAMPlace GP 在 ~500 iterations 后就停滞不动**：iter0500 ~ iter0999 的 plot 图档完全相同（档案大小一致为 1,295,277 bytes），说明 optimizer 提前卡在 local minimum，尚未达到 `stop_overflow = 0.1` 的停止条件就已无法继续优化。

2. **Cell 密度分布极不均匀**：从 `dreamplace_placement.png` 可以清楚看到 standard cells 在中央和左侧形成密集团簇（clump），右侧和底部有大片空白区域。这种局部密度热点是 routing congestion 的直接原因。

3. **Global routing 跑满 60 iterations 未收敛**：`5_1_grt.log` 显示 GRT 的 congestion removal iterations 跑到 32/60（log 截断处）仍在继续，说明 overflow 无法消除。

4. **Detailed placement 位移过大**：`3_5_place_dp.log` 显示 average displacement = 4.1 um，max displacement = 74.3 um，legalized HPWL 比 GP HPWL 增加 13%，说明 DREAMPlace GP 结果的 legalizability 不佳。

## 根本原因

当前 [`mempool_group_fenced.json`](flow/scripts/dreamplace_bridge/mempool_group_fenced.json) 的参数组合导致 density spreading 不足：

- `density_weight: 8e-5` — 密度惩罚权重太低，cells 无法被有效推开
- `num_bins_x/y: 512` — bin 解析度过高（每 bin ~2.15 um），密度场太 local，无法形成全域 spreading 压力
- `gamma: 4.0` — wirelength smoothing 系数偏低，梯度容易陷入 local minimum
- `stop_overflow: 0.1` — 虽然合理，但 GP 在达到此目标前就已停滞
- `ignore_net_degree: 100` — 设计中有 257-pin 的大 net 被忽略，无法优化其 wirelength/congestion
- 只有 1 个 `global_place_stage` — 缺乏 coarse-to-fine 的多阶段优化
- `routability_opt_flag` 未启用 — 没有基于 RUDY congestion map 的 cell inflation 优化

## 参数调整方案

修改 [`mempool_group_fenced.json`](flow/scripts/dreamplace_bridge/mempool_group_fenced.json)，具体变更如下：

### 1. 启用 routability optimization（最重要）

```json
"routability_opt_flag": 1,
"adjust_rudy_area_flag": 1,
"adjust_pin_area_flag": 1,
"node_area_adjust_overflow": 0.20,
"max_num_area_adjust": 5,
"route_num_bins_x": 256,
"route_num_bins_y": 256
```

这会让 DREAMPlace 在 GP 过程中根据 RUDY（Rectangular Uniform wire DensitY）和 pin utilization map 对 congested 区域的 cell 进行 area inflation，自动缓解 routing 热点。`node_area_adjust_overflow` 从默认 0.15 放宽到 0.20，让 routability 调整更早介入。

### 2. 改用 coarse-to-fine 两阶段 global placement

```json
"global_place_stages": [
    {
        "num_bins_x": 256,
        "num_bins_y": 256,
        "iteration": 800,
        "learning_rate": 0.01,
        "wirelength": "weighted_average",
        "optimizer": "nesterov"
    },
    {
        "num_bins_x": 512,
        "num_bins_y": 512,
        "iteration": 800,
        "learning_rate": 0.005,
        "wirelength": "weighted_average",
        "optimizer": "nesterov"
    }
]
```

- 第一阶段使用 256x256 bins（每 bin ~4.3 um）：coarser density field 产生更强的全域 spreading 效果，先让 cells 大致均匀分布
- 第二阶段使用 512x512 bins + 较低 learning rate：在全域分布合理的基础上做精细调整
- 总 iteration 数增加到 1600，给 optimizer 更多空间收敛

### 3. 提高 density weight

```json
"density_weight": 8e-4
```

从 `8e-5` 提高 10 倍至 `8e-4`。这是 density penalty 的初始权重，更高的值会在优化初期就施加更强的 spreading 压力，避免 cells 在早期就聚集成团簇。

### 4. 降低 stop_overflow

```json
"stop_overflow": 0.07
```

从 0.1 降到 0.07，要求 DREAMPlace 将 density overflow 压得更低再停止。这会让 placement 更均匀，减少局部拥塞。（DREAMPlace 默认示例 `adaptec1.json` 也是 0.07。）

### 5. 提高 ignore_net_degree

```json
"ignore_net_degree": 300
```

从 100 提高到 300。设计中有 16 条 257-pin 的大 net（icache prefetcher 相关），目前被完全忽略。将这些 net 纳入 wirelength 优化后，DREAMPlace 会尝试缩短它们的 HPWL，有助于减少这些大 net 的 routing congestion。

### 6. 略微增大 gamma

```json
"gamma": 6.0
```

从 4.0 增加到 6.0。`gamma` 控制 weighted-average wirelength 的 smoothing 程度（相对于 bin size），较高的值让 wirelength 函数更平滑，减少梯度震荡，帮助 optimizer 跳出 local minimum。

### 7. 减小顶层 num_bins

```json
"num_bins_x": 256,
"num_bins_y": 256
```

顶层 `num_bins` 与第一阶段一致，避免冲突。

## 最终完整 JSON

```json
{
    "lef_input": [
        "/home/yenchulo/OpenROAD-flow-scripts/flow/platforms/nangate45/lef/NangateOpenCellLibrary.tech.lef",
        "/home/yenchulo/OpenROAD-flow-scripts/flow/platforms/nangate45/lef/NangateOpenCellLibrary.macro.mod.lef",
        "/home/yenchulo/OpenROAD-flow-scripts/flow/platforms/nangate45/lef/fakeram45_256x32.lef",
        "/home/yenchulo/OpenROAD-flow-scripts/flow/platforms/nangate45/lef/fakeram45_64x64.lef"
    ],
    "def_input": "/home/yenchulo/OpenROAD-flow-scripts/flow/results/nangate45/mempool_group/base/2_floorplan_fenced.def",
    "verilog_input": "/home/yenchulo/OpenROAD-flow-scripts/flow/results/nangate45/mempool_group/base/2_floorplan_for_dp.v",
    "gpu": 0,
    "num_bins_x": 256,
    "num_bins_y": 256,
    "global_place_stages": [
        {
            "num_bins_x": 256,
            "num_bins_y": 256,
            "iteration": 800,
            "learning_rate": 0.01,
            "wirelength": "weighted_average",
            "optimizer": "nesterov"
        },
        {
            "num_bins_x": 512,
            "num_bins_y": 512,
            "iteration": 800,
            "learning_rate": 0.005,
            "wirelength": "weighted_average",
            "optimizer": "nesterov"
        }
    ],
    "target_density": 0.4,
    "density_weight": 8e-4,
    "gamma": 6.0,
    "random_seed": 1000,
    "ignore_net_degree": 300,
    "enable_fillers": 1,
    "gp_noise_ratio": 0.025,
    "global_place_flag": 1,
    "legalize_flag": 1,
    "detailed_place_flag": 0,
    "stop_overflow": 0.07,
    "dtype": "float32",
    "result_dir": "/home/yenchulo/OpenROAD-flow-scripts/flow/results/nangate45/mempool_group/base/dreamplace_results",
    "num_threads": 8,
    "plot_flag": 1,
    "random_center_init_flag": 1,
    "sort_nets_by_degree": 0,
    "routability_opt_flag": 1,
    "adjust_rudy_area_flag": 1,
    "adjust_pin_area_flag": 1,
    "node_area_adjust_overflow": 0.20,
    "max_num_area_adjust": 5,
    "route_num_bins_x": 256,
    "route_num_bins_y": 256
}
```

## 变更汇总

| 参数 | 旧值 | 新值 | 目的 |
|---|---|---|---|
| `density_weight` | 8e-5 | 8e-4 | 增强 spreading 压力 |
| `gamma` | 4.0 | 6.0 | 平滑 wirelength，避免 local minimum |
| `num_bins_x/y` | 512 | 256 | coarser density field，全域 spreading |
| `stop_overflow` | 0.1 | 0.07 | 压低 density overflow |
| `ignore_net_degree` | 100 | 300 | 纳入 257-pin 大 net 的优化 |
| `global_place_stages` | 1 stage / 1000 iter | 2 stages / 1600 iter total | coarse-to-fine 优化 |
| `routability_opt_flag` | 未设定 (0) | 1 | 启用 RUDY/pin congestion 调整 |
| `node_area_adjust_overflow` | 未设定 (0.15) | 0.20 | routability 调整更早介入 |
| `max_num_area_adjust` | 未设定 (3) | 5 | 允许更多次 area inflation 迭代 |

## 预期效果

- Cells 在 die area 上更均匀分布，消除当前的密集团簇
- RUDY-based cell inflation 自动缓解 routing 热点
- Legalization 的 displacement 应显著下降（目标 < 2 um average）
- Global routing 应能在合理迭代数内收敛

## 若仍不收敛的后续调整

如果上述调整后 GRT 仍有 overflow，可进一步尝试：
- 将 `density_weight` 再提高至 `1e-3`
- 将 `stop_overflow` 降到 `0.05`
- 在 `global_place_stages` 中增加第三阶段
- 启用 `deterministic_flag: 1` 确保结果可重现以便调参
