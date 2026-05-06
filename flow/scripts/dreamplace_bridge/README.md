# OpenROAD-DREAMPlace Bridge

將 OpenROAD floorplan 的結果匯出，以 DREAMPlace 取代 OpenROAD 內建的 placement（stages 3\_1–3\_5），
並透過 RTLMP cluster 邊界作為 fence region 約束 standard cell 的擺放範圍，
最後將 DREAMPlace 結果匯回 OpenROAD 繼續 CTS / routing / finishing。

```
OpenROAD floorplan (stages 1-2)
  --> export DEF + Verilog
  --> inject RTLMP fence regions into DEF
  --> DREAMPlace placement (with fence constraints, replaces 3_1 + 3_3)
  --> import placement back into OpenROAD
  --> 3_2 IO placement
  --> skip 3_3 global placement (DREAMPlace already did it)
  --> 3_4 resize / 3_5 detail placement
  --> continue CTS / route / finish (stages 4-6)
```

## 目錄結構

| 檔案 | 說明 |
|---|---|
| `export_for_dreamplace.tcl` | 從 OpenROAD `2_floorplan.odb` 匯出 DEF + Verilog |
| `inject_fence_regions.py` | 解析 RTLMP 報告，將 REGIONS/GROUPS (TYPE FENCE) 注入 DEF |
| `mempool_group_fenced.json` | DREAMPlace JSON 設定檔（nangate45 / mempool\_group） |
| `import_dreamplace.tcl` | 將 DREAMPlace 輸出的 placement 匯入 OpenROAD ODB |
| `run_dreamplace_flow.sh` | 一鍵執行完整 pipeline 的主腳本 |
| `plot_placement.py` | 視覺化 DREAMPlace placement 結果（standard cell 分布 + macro 位置） |

---

## 前置條件

### 1. 產生 RTLMP clustering 報告

DREAMPlace 的 fence region 來自 OpenROAD floorplan 中 RTLMP（RTL Macro Placer）
產出的 cluster 資料。你需要先完成 floorplan 並確保以下兩個檔案存在：

- `reports/<platform>/<design>/base/rtlmp_clusters.csv`
- `reports/<platform>/<design>/base/rtlmp_instance_to_cluster.txt`

#### 方法：使用 OpenROAD-flow-scripts 執行 floorplan

在 `config.mk` 中設定以下變數以啟用 RTLMP clustering 資料輸出：

```makefile
# 保留 clustering 資料（必須）
export RTLMP_KEEP_CLUSTERING_DATA = 1

# 控制最大 clustering 層級
export RTLMP_MAX_LEVEL = 1

# 其他 RTLMP 參數（依設計調整）
export RTLMP_MIN_AR = 0.33
export RTLMP_AREA_WT = 0.1
export RTLMP_WIRELENGTH_WT = 100
export RTLMP_OUTLINE_WT = 100
export RTLMP_BOUNDARY_WT = 50
export RTLMP_NOTCH_WT = 50
```

完整範例請參考 `designs/nangate45/mempool_group/config.mk`。

接著執行 floorplan：

```bash
cd /home/yenchulo/OpenROAD-flow-scripts/flow

# 執行完整 floorplan（synth + floorplan stages 1-2）
make floorplan DESIGN_CONFIG=./designs/nangate45/mempool_group/config.mk
```

完成後會產生：

```
results/nangate45/mempool_group/base/
  2_floorplan.odb          # floorplan 資料庫
  2_floorplan.sdc          # timing constraints

reports/nangate45/mempool_group/base/
  rtlmp_clusters.csv       # cluster 名稱、bounding box（um/dbu）
  rtlmp_clusters.png       # cluster 視覺化圖（可選）
  rtlmp_instance_to_cluster.txt  # instance -> cluster 對應表
```

### 2. DREAMPlace 環境

- DREAMPlace 安裝目錄：`/home/yenchulo/DREAMPlace/install/`
- Conda 環境：`DREAMPlace`

```bash
# 確認 conda 環境可用
source /home/yenchulo/anaconda3/etc/profile.d/conda.sh
conda activate DREAMPlace
python -c "import dreamplace; print('OK')"
```

> **注意**：若系統無 CUDA 11 runtime，需將
> `DREAMPlace/install/dreamplace/configure.py` 中的
> `"CUDA_FOUND": "TRUE"` 改為 `"FALSE"`，並在 JSON 設定中設 `"gpu": 0`。

---

## 執行方式

### 一鍵執行（推薦）

```bash
cd /home/yenchulo/OpenROAD-flow-scripts/flow/scripts/dreamplace_bridge

# 只跑 placement pipeline（產生 3_1_place_gp_skip_io.odb）
bash run_dreamplace_flow.sh

# placement 完成後自動接續 OpenROAD CTS / route / finish
bash run_dreamplace_flow.sh --continue-openroad
```

### 分步執行

如果需要手動控制每個步驟：

#### Step 1：匯出 DEF + Verilog

```bash
export RESULTS_DIR=/home/yenchulo/OpenROAD-flow-scripts/flow/results/nangate45/mempool_group/base

openroad -no_splash -no_init export_for_dreamplace.tcl
```

輸出：
- `$RESULTS_DIR/2_floorplan_for_dp.def`
- `$RESULTS_DIR/2_floorplan_for_dp.v`

#### Step 2：注入 fence regions

```bash
python3 inject_fence_regions.py \
    --clusters-csv  ../../reports/nangate45/mempool_group/base/rtlmp_clusters.csv \
    --instance-map  ../../reports/nangate45/mempool_group/base/rtlmp_instance_to_cluster.txt \
    --def-input     $RESULTS_DIR/2_floorplan_for_dp.def \
    --def-output    $RESULTS_DIR/2_floorplan_fenced.def
```

此腳本會：
1. 從 CSV 中讀取 `kind == "cluster"` 的 38 個 cluster bounding box
2. 從 instance mapping 中讀取 ~164K instance 的 cluster 歸屬
3. 移除原始 DEF 中的 SPECIALNETS、既有 GROUPS/REGIONS
4. 注入新的 `REGIONS` (TYPE FENCE) + `GROUPS` section

#### Step 3：執行 DREAMPlace

```bash
source /home/yenchulo/anaconda3/etc/profile.d/conda.sh
conda activate DREAMPlace
cd /home/yenchulo/DREAMPlace/install

python dreamplace/Placer.py \
    /home/yenchulo/OpenROAD-flow-scripts/flow/scripts/dreamplace_bridge/mempool_group_fenced.json
```

輸出 DEF 位於：
`$RESULTS_DIR/dreamplace_results/2_floorplan_for_dp/2_floorplan_for_dp.gp.def`

#### Step 4：匯入 placement 回 OpenROAD

```bash
export DREAMPLACE_DEF=$RESULTS_DIR/dreamplace_results/2_floorplan_for_dp/2_floorplan_for_dp.gp.def

openroad -no_splash -no_init import_dreamplace.tcl
```

輸出：
- `$RESULTS_DIR/3_1_place_gp_skip_io.odb`
- `$RESULTS_DIR/3_1_place_gp_skip_io.sdc`

#### Step 5（可選）：繼續 OpenROAD flow（跳過 3\_3）

```bash
cd /home/yenchulo/OpenROAD-flow-scripts/flow
DESIGN_CFG=./designs/nangate45/mempool_group/config.mk

# 3_2: IO placement
make do-3_2_place_iop DESIGN_CONFIG=$DESIGN_CFG

# 跳過 3_3: 將 3_2 output 作為 3_3 output（DREAMPlace 已完成 global placement）
cp $RESULTS_DIR/3_2_place_iop.odb $RESULTS_DIR/3_3_place_gp.odb

# 3_4 resize + 3_5 detail placement
make do-3_4_place_resized do-3_5_place_dp do-3_place do-3_place.sdc DESIGN_CONFIG=$DESIGN_CFG

# 4-6: CTS, routing, finish
make do-cts do-route do-finish DESIGN_CONFIG=$DESIGN_CFG
```

---

## DREAMPlace JSON 設定參數

`mempool_group_fenced.json` 中的關鍵參數：

| 參數 | 值 | 說明 |
|---|---|---|
| `target_density` | 0.4 | 需與 OpenROAD `PLACE_DENSITY` 一致 |
| `gpu` | 0 | 0=CPU, 1=GPU（需 CUDA） |
| `legalize_flag` | 1 | 啟用 legalization |
| `detailed_place_flag` | 0 | 不做 detailed placement |
| `stop_overflow` | 0.1 | global placement 停止條件 |
| `global_place_stages` | [...] | 控制迭代次數、learning rate 等 |

---

## 視覺化 Placement 結果

`plot_placement.py` 可將 DREAMPlace 輸出的 DEF 檔案繪製為 placement 分布圖，
包含 standard cell 散點分布與 macro block 矩形位置。

### 基本用法

```bash
export RESULTS_DIR=/home/yenchulo/OpenROAD-flow-scripts/flow/results/nangate45/mempool_group/base

python3 plot_placement.py \
    --def-input $RESULTS_DIR/dreamplace_results/2_floorplan_for_dp/2_floorplan_for_dp.gp.def \
    --lef-dir ../../platforms/nangate45/lef \
    --output $RESULTS_DIR/dreamplace_placement.png
```

### 參數說明

| 參數 | 預設值 | 說明 |
|---|---|---|
| `--def-input` | （必填） | DREAMPlace 輸出的 DEF 檔案路徑 |
| `--lef-dir` | （必填） | LEF 檔案目錄（用於解析 macro 尺寸） |
| `--output` | `<def_stem>_placement.png` | 輸出圖片路徑 |
| `--dpi` | 300 | 圖片解析度 |
| `--figsize` | 12.0 | 圖片寬度（英寸） |

### 輸出範例

圖片會顯示：
- **紫色散點**：standard cell 的擺放位置（~164K cells）
- **紅色矩形**：macro block（SRAM 等）的位置與大小
- **深色背景**：die area 邊界

> 此腳本也可用於非 DREAMPlace 的 DEF 檔案，只要 DEF 格式符合 LEF/DEF 5.x 標準即可。

---

## 適配其他設計

若要用於其他 platform/design，需修改：

1. **`run_dreamplace_flow.sh`**：修改 `PLATFORM`、`DESIGN`、`VARIANT` 變數
2. **`mempool_group_fenced.json`**：更新 `lef_input`（對應 platform 的 LEF）、`def_input`、`verilog_input` 路徑，以及 `target_density`
3. **`config.mk`**：確保目標設計的 config 有設定 `RTLMP_KEEP_CLUSTERING_DATA = 1`

---

## 已知限制

- **Timing**：DREAMPlace 不做 timing-driven resizing。如需要，可在 import 後手動執行 OpenROAD 的 `resize` 步驟
- **Detailed placement**：目前 `detailed_place_flag = 0`，如需啟用需確認 DREAMPlace 的 detailed placer 相容性
- **PDN**：SPECIALNETS 在注入 fence 時被移除（DREAMPlace 不需要），但 PDN 資訊保留在原始 ODB 中，import 時不受影響
- **Unconstrained cells**：`kind == "point"` 的 cluster 不建立 fence，其中的 instance 由 DREAMPlace 自由擺放
