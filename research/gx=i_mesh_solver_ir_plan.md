---
name: Gx=i mesh solver IR
overview: Add a conductance-matrix mesh solve (Gx=i) as Step 1 to compute mesh node voltages, then use the paper's max-mode local drop formulas (Step 2) for hard macro P/G pins and soft module worst-case voltages.
todos:
  - id: cg-solver
    content: Implement matrix-free CG solver (matvec_G + solve_mesh_cg) in pdn_ir.cpp with 5-point stencil, ring boundary conditions, and Jacobi preconditioner
    status: completed
  - id: solve-api
    content: Add solveMeshVoltages() public method to IrModel in pdn_ir.hpp
    status: completed
  - id: update-voltages
    content: Rewrite updateMeshVoltages() to call solveMeshVoltages() when ir_mode==Max, keep old lumped path for ir_mode==Sum
    status: completed
  - id: hard-pin
    content: "Rewrite hard_pin_V(): use solved V_i + max local drop from mesh node to pin (Dx/Dy = pin-to-node distance)"
    status: completed
  - id: soft-tile
    content: "Rewrite soft_tile_worst_V(): use solved V_i + max local drop from mesh node to farthest overlap corner"
    status: completed
  - id: node-voltage
    content: Simplify nodeVoltage() to return mesh_.nodes[idx].v_V (already solved by CG)
    status: completed
  - id: cli-default
    content: Change default ir_mode to max in main.cpp and update readme.md
    status: completed
isProject: false
---

# Two-Step IR Drop: Gx=i Mesh Solve + Local Drop Formulas

## Background

The current code computes each mesh node's voltage independently via a **lumped formula**: `V = V_ring - I * R(node -> nearest_ring)`. This bypasses any network coupling between mesh nodes.

The paper's approach is a **two-step** process:

1. **Step 1 (Mesh Solve)**: Build a conductance matrix G from the strap network, inject load currents at mesh nodes, fix ring nodes at VDD, and solve **Gx = i** to get the voltage V_i at every mesh node. This captures the coupled effect of all loads on the shared resistive network.

2. **Step 2 (Local Drop)**: Use the solved V_i as the reference voltage, then compute the additional IR drop from each mesh node to the actual load location (pin or tile region):
   - **Hard macro P/G pin j** with nearest mesh node i:
     \[V_j = V_i - I_j \cdot \max\!\left(r_\text{sqh}\frac{Dx_{ij}}{w_\text{hstrap}},\; r_\text{sqv}\frac{Dy_{ij}}{w_\text{vstrap}}\right)\]
   - **Soft module k**, worst-case over overlapping tiles S_ov:
     \[V_k = \min_{S_\text{ov}}\!\left(V_i - I_{k,i}\cdot\max\!\left(r_\text{sqh}\frac{Dx_{ik}}{w_\text{hstrap}},\; r_\text{sqv}\frac{Dy_{ik}}{w_\text{vstrap}}\right)\right)\]

   Here Dx/Dy are the distances from the load (pin or farthest overlap corner) to the mesh node -- **not** to the ring node. The local drop uses **max** (not sum).

## Key Differences from Current Code

- **Current**: `nodeVoltage()` = `V_ring - I * R(node -> ring)` -- independent per node, no coupling.
- **Paper**: `nodeVoltage()` comes from solving Gx=i -- all loads interact through the shared mesh resistance.
- **Current**: `hard_pin_V()` and `soft_tile_worst_V()` compute the full drop from ring to pin/tile.
- **Paper**: These functions compute only the **local** drop from the nearest mesh node (which already has its solved V_i) to the pin/tile.
- **Current**: `ir_drop()` uses `Sum` mode (Rh+Rv) as default.
- **Paper**: The local drop formulas explicitly use `max(Rh, Rv)`.

## Implementation Plan

### 1. Add CG Mesh Solver to `pdn_ir.cpp`

Add a matrix-free Conjugate Gradient solver operating on the 5-point stencil of the structured mesh.

**Conductance model** (between adjacent mesh nodes):
- Horizontal neighbors (ix, iy) <-> (ix+1, iy): `g_h = w_hstrap / (r_sqh * dx)` where `dx = |x[ix+1] - x[ix]|`
- Vertical neighbors (ix, iy) <-> (ix, iy+1): `g_v = w_vstrap / (r_sqv * dy)` where `dy = |y[iy+1] - y[iy]|`

This naturally handles both uniform (synth) and non-uniform (odb) meshes.

**Boundary conditions**: Ring nodes are fixed at VDD. Implemented by zeroing off-diagonal entries and setting diagonal to 1 with RHS = VDD for ring node rows.

**RHS current vector**: `i[n] = node.I_soft_A + node.I_hard_A` at each internal node (convention: positive current drawn from supply into the node, as consumed by loads).

**Solver**: CG with Jacobi (diagonal) preconditioner. For typical mesh sizes (few hundred nodes), this converges in tens of iterations. Add a configurable tolerance (default 1e-10) and max iteration count (default 5000).

**New functions in anonymous namespace** of [research/src/pdn_ir.cpp](research/src/pdn_ir.cpp):
- `matvec_G(mesh, strap, x, Gx)` -- matrix-free G*x product using 5-point stencil
- `solve_mesh_cg(mesh, strap, tol, max_iter)` -- CG loop; writes solved voltages into `mesh.nodes[*].v_V`

### 2. Add `solveMeshVoltages()` to `IrModel`

New public method in [research/include/phys/pdn_ir.hpp](research/include/phys/pdn_ir.hpp):
- `void solveMeshVoltages()` -- calls `solve_mesh_cg`, stores result in `mesh_.nodes[*].v_V`

### 3. Modify `updateMeshVoltages()`

Change [research/src/pdn_ir.cpp](research/src/pdn_ir.cpp) `IrModel::updateMeshVoltages()` to call `solveMeshVoltages()` instead of the per-node lumped formula.

### 4. Rewrite `hard_pin_V()`

In [research/src/pdn_ir.cpp](research/src/pdn_ir.cpp), change `hard_pin_V()`:
- Find nearest mesh node i to pin (x_j, y_j) -- same as current
- Read `V_i = mesh.nodes[i].v_V` (already solved from Step 1)
- Compute `Dx = |pin_x - node_i.x|`, `Dy = |pin_y - node_i.y|`
- Local drop = `I_j * max(r_sqh * Dx / w_h, r_sqv * Dy / w_v)` (always **max** mode)
- Return `V_i - local_drop`

### 5. Rewrite `soft_tile_worst_V()`

In [research/src/pdn_ir.cpp](research/src/pdn_ir.cpp), change `soft_tile_worst_V()`:
- For each tile i overlapping the module box:
  - Read `V_i = mesh.nodes[i].v_V` (solved)
  - Compute `I_{k,i} = kn.imax(A_overlap)`
  - Compute Dx, Dy as the max distance from mesh node i to the **farthest corner** of the overlap region (worst case within tile)
  - Local drop = `I_{k,i} * max(r_sqh * Dx / w_h, r_sqv * Dy / w_v)` (always **max**)
  - `V_tile = V_i - local_drop`
- Return `min(V_tile)` over all overlapping tiles

### 6. Simplify `nodeVoltage()`

Change `IrModel::nodeVoltage()` to simply return `mesh_.nodes[node_idx].v_V` (the already-solved value), since the voltage is now computed by the CG solver rather than per-node lumped formula.

### 7. Update `IrMode` Semantics / CLI

The `IrMode` enum's original purpose was to choose between Sum and Max for the lumped formula. With the two-step approach:
- The **mesh solve** (Step 1) does not use IrMode -- it uses the physical conductance matrix.
- The **local drop** (Step 2) always uses `max` per the paper formulas.

Options:
- Keep `--ir-mode sum` as a fallback that preserves the old lumped behavior (no Gx=i solve).
- Default to `max` which activates the new two-step paper flow.
- Or remove `--ir-mode` entirely and always use the two-step approach.

Recommended: keep both modes for comparison. `--ir-mode max` (new default) = two-step with Gx=i. `--ir-mode sum` = legacy lumped.

### 8. Update README and Docs

Update [research/readme.md](research/readme.md) to reflect:
- `--ir-mode max` (default, paper): two-step Gx=i + local max drop
- `--ir-mode sum` (legacy): lumped sum to ring (no mesh solve)

## Files Changed

- [research/include/phys/pdn_ir.hpp](research/include/phys/pdn_ir.hpp) -- add `solveMeshVoltages()`, update comments/docstrings
- [research/src/pdn_ir.cpp](research/src/pdn_ir.cpp) -- add CG solver, rewrite `hard_pin_V`, `soft_tile_worst_V`, `updateMeshVoltages`, `nodeVoltage`
- [research/src/main.cpp](research/src/main.cpp) -- change default `ir_mode_str` from `"sum"` to `"max"`, update log messages
- [research/readme.md](research/readme.md) -- update CLI docs

No new dependencies required. No changes to CMakeLists.txt.
