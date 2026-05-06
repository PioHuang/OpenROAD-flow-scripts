---
name: Refine vsrc placement
overview: Modify voltage source placement in all three mesh modes (multilayer, odb, synth) so that voltage sources are placed only at physically meaningful entry points -- BTERM-die-boundary intersections for multilayer, and stripe-ring intersections for odb/synth -- rather than along the entire BTERM/ring.
todos:
  - id: multilayer-bterm
    content: Modify multilayer BTERM vsrc placement (pdn_ir.cpp ~1305-1326) to only mark nodes at BTERM-die-boundary intersections
    status: completed
  - id: odb-ring
    content: Modify ODB ring vsrc placement (pdn_ir.cpp ~729-750) to only mark nodes at stripe-ring intersections, tracking stripe_xs/stripe_ys separately from ring coordinates
    status: completed
  - id: synth-ring
    content: Add stripe-aware ring marking for synth mode using Tech::stripes positions, with fallback to mark_outer_ring
    status: completed
  - id: test-verify
    content: Build and run phys_load on mempool_group to verify the vsrc count changes and mesh voltages remain reasonable
    status: completed
isProject: false
---

# Restrict Voltage Sources to Boundary/Intersection Points

## Background

Currently, voltage sources are placed on **all** mesh nodes that fall within a RING or BTERM AABB. This overly generous placement does not model the physical reality where power enters at discrete points (die boundary pins for BTERMs, strap-ring junctions for rings).

All changes are in [`research/src/pdn_ir.cpp`](research/src/pdn_ir.cpp). Minor signature changes may touch [`research/include/phys/pdn_ir.hpp`](research/include/phys/pdn_ir.hpp) if needed.

---

## 1. Multilayer BTERM: Only at die-boundary endpoints

**Current code** (lines 1305-1326): For each node on the pin layer inside any BTERM AABB, mark as vsrc.

**New logic**: For each BTERM segment, determine orientation and only mark nodes at the positions where the BTERM meets the core boundary (i.e., the "die edge").

- Determine BTERM orientation: horizontal if `(xhi - xlo) > (yhi - ylo)`, vertical otherwise
- For a horizontal BTERM: check if its `xlo` is near `core lx` or its `xhi` is near `core ux` (within a tolerance, e.g. 1 um). For each die-edge endpoint that the BTERM reaches, find the mesh node **inside the BTERM** with the closest coordinate along the stripe direction to that core boundary, and mark only that node as vsrc
- For a vertical BTERM: same logic but in the y-direction against `core ly` / `core uy`
- Partial BTERMs (e.g., `x: 10 -> 236`) only touch one die edge, so only one endpoint node gets marked

The core boundary (`lx, ly, ux, uy`) is already available in `build_multilayer_mesh` (line 959-962).

**Example**: A horizontal BTERM from x=10.07 to x=1089.84 at y~549. Core lx~10, ux~1090. Both endpoints reach the die edge. Among the ~35 mesh nodes along this BTERM, only the leftmost (x~27) and rightmost (x~1077) get voltage sources. The remaining ~33 interior nodes are left as normal internal nodes.

---

## 2. ODB RING: Only at stripe-ring intersections

**Current code** (lines 729-750): For each node inside any RING AABB, mark as vsrc.

**New logic**: Among nodes inside a RING AABB, additionally require that the node is at a **stripe crossing** -- where a STRIPE passes through the ring.

Implementation approach in `build_mesh_from_pdn_dump`:
- While iterating dump segments (lines 650-681), collect two additional sets:
  - `stripe_xs`: x-centers of vertical STRIPE segments (already computed as `xs` entries, but we need to track them separately from RING contributions)
  - `stripe_ys`: y-centers of horizontal STRIPE segments (same, separately from RING)
- When marking ring nodes (lines 735-749), add an intersection check:
  - For a horizontal RING segment: a node is a vsrc only if `node.x_um` matches a value in `stripe_xs` (within tolerance, e.g. 0.5 um)
  - For a vertical RING segment: a node is a vsrc only if `node.y_um` matches a value in `stripe_ys`
- This means: which RING type the node is inside determines the check direction. We iterate `rings` and for each matching ring, check its orientation and apply the cross-direction stripe test

**Corner case**: If no stripe-ring intersections are found (e.g., design has ring but no stripes), fall back to the existing `mark_outer_ring` (line 752-753).

---

## 3. Synth RING: Only at stripe-ring intersections

**Current code**: `buildUniformMesh` (line 1652-1656) calls `mark_outer_ring`, which marks **all** outermost boundary nodes.

**New logic**: Use the actual stripe positions from `Tech::stripes` to determine which outer-ring nodes should be vsrc.

Implementation approach:
- Create a new helper function (e.g., `mark_ring_at_stripe_crossings`) that accepts stripe pitch/offset information
- From `chip_->tech.stripes`, extract vertical stripe x-positions and horizontal stripe y-positions
- On the top/bottom outer ring: only mark nodes whose `x_um` matches a vertical stripe position
- On the left/right outer ring: only mark nodes whose `y_um` matches a horizontal stripe position
- If `Tech::stripes` is empty or no intersections are found, fall back to `mark_outer_ring` (current behavior)

**Note**: When the mesh pitch exactly equals the stripe pitch (common case), every outer-ring node IS at a stripe intersection, so behavior is unchanged. This modification matters when the mesh is at a finer/different resolution than the stripe pitch.

The `buildUniformMesh` method lives in `IrModel`, which has access to `chip_->tech.stripes`. The stripe offset/pitch can be used to generate the set of stripe positions clipped to the core region.

---

## Design decisions and notes

- **Core vs die boundary for BTERMs**: Using `layout.core` (not `layout.die`) since the mesh is built on the core region and nodes are clipped to it. BTERMs that extend to the die edge will have their nearest mesh node near the core edge.
- **Multilayer RING (Priority 1)** is left unchanged -- the user's request targets BTERMs for multilayer mode. If desired, the same stripe-ring intersection logic from ODB could be applied to multilayer RING handling later.
- **Priority 3 fallback** (outermost nodes on pin layer) is left unchanged -- it is a last-resort when neither RING nor BTERM segments exist.
- **Tolerance**: The BTERM-to-boundary proximity check uses a tolerance like 1-2 um (slightly larger than the existing 0.5 um point-in-AABB slack). The stripe-intersection check reuses the existing 0.5 um slack.
