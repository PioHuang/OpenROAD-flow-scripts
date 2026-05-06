#include <phys/pdn_ir.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phys {

namespace {

// ---- helpers shared by single-layer and multi-layer paths ----

std::vector<std::string> fp_tokens(const std::string& name) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < name.size()) {
    const size_t q = name.find("||", p);
    std::string tok
        = (q == std::string::npos) ? name.substr(p) : name.substr(p, q - p);
    if (!tok.empty())
      out.push_back(std::move(tok));
    if (q == std::string::npos)
      break;
    p = q + 2;
  }
  return out;
}

bool fp_is_hard(const Chip& chip, const FpBox& box) {
  const auto toks = fp_tokens(box.name);
  if (toks.empty())
    return false;
  for (const auto& t : toks) {
    auto it = chip.instances.find(t);
    if (it == chip.instances.end())
      return false;
    if (!it->second.is_macro)
      return false;
  }
  return true;
}

double fp_area_um2(const FpBox& b) {
  return std::max(0.0, b.ux - b.lx) * std::max(0.0, b.uy - b.ly);
}

const FpBox* find_fp(const std::vector<FpBox>& fp, const std::string& name) {
  for (const auto& b : fp) {
    if (b.name == name)
      return &b;
  }
  return nullptr;
}

double rect_overlap_um2(double ax0,
                        double ay0,
                        double ax1,
                        double ay1,
                        double bx0,
                        double by0,
                        double bx1,
                        double by1,
                        double* ox0 = nullptr,
                        double* oy0 = nullptr,
                        double* ox1 = nullptr,
                        double* oy1 = nullptr) {
  const double x0 = std::max(ax0, bx0);
  const double y0 = std::max(ay0, by0);
  const double x1 = std::min(ax1, bx1);
  const double y1 = std::min(ay1, by1);
  const double w = std::max(0.0, x1 - x0);
  const double h = std::max(0.0, y1 - y0);
  if (ox0) {
    *ox0 = x0;
    *oy0 = y0;
    *ox1 = x1;
    *oy1 = y1;
  }
  return w * h;
}

void node_tile_um(int ix,
                  int iy,
                  int nx,
                  int ny,
                  double core_lx,
                  double core_ly,
                  double core_ux,
                  double core_uy,
                  double pitch_x_um,
                  double pitch_y_um,
                  double mesh_lx,
                  double mesh_ly,
                  double* tx0,
                  double* ty0,
                  double* tx1,
                  double* ty1) {
  *tx0 = (ix == 0) ? core_lx : (mesh_lx + (ix - 0.5) * pitch_x_um);
  *tx1 = (ix == nx - 1) ? core_ux : (mesh_lx + (ix + 0.5) * pitch_x_um);
  *ty0 = (iy == 0) ? core_ly : (mesh_ly + (iy - 0.5) * pitch_y_um);
  *ty1 = (iy == ny - 1) ? core_uy : (mesh_ly + (iy + 0.5) * pitch_y_um);
}

// Non-uniform version: tile boundaries from actual coordinate arrays.
void node_tile_nu(int ix,
                  int iy,
                  const std::vector<double>& xs,
                  const std::vector<double>& ys,
                  double core_lx,
                  double core_ly,
                  double core_ux,
                  double core_uy,
                  double* tx0,
                  double* ty0,
                  double* tx1,
                  double* ty1) {
  const int nx = static_cast<int>(xs.size());
  const int ny = static_cast<int>(ys.size());
  if (ix == 0)
    *tx0 = core_lx;
  else
    *tx0 = 0.5 * (xs[static_cast<size_t>(ix - 1)]
                  + xs[static_cast<size_t>(ix)]);
  if (ix == nx - 1)
    *tx1 = core_ux;
  else
    *tx1 = 0.5 * (xs[static_cast<size_t>(ix)]
                  + xs[static_cast<size_t>(ix + 1)]);
  if (iy == 0)
    *ty0 = core_ly;
  else
    *ty0 = 0.5 * (ys[static_cast<size_t>(iy - 1)]
                  + ys[static_cast<size_t>(iy)]);
  if (iy == ny - 1)
    *ty1 = core_uy;
  else
    *ty1 = 0.5 * (ys[static_cast<size_t>(iy)]
                  + ys[static_cast<size_t>(iy + 1)]);
}

double ir_drop(const StrapSheetModel& strap,
               double dx_um,
               double dy_um,
               double I_A,
               IrMode mode) {
  const double rh = (strap.w_hstrap_um > 0.0)
                        ? (strap.r_sqh * dx_um / strap.w_hstrap_um)
                        : std::numeric_limits<double>::infinity();
  const double rv = (strap.w_vstrap_um > 0.0)
                        ? (strap.r_sqv * dy_um / strap.w_vstrap_um)
                        : std::numeric_limits<double>::infinity();
  if (mode == IrMode::Max)
    return I_A * std::max(rh, rv);
  return I_A * (rh + rv);
}

SoftKnapsackProfile make_profile_from_items(
    std::vector<std::pair<double, double>> items) {
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
    return (a.first / a.second) > (b.first / b.second);
  });
  SoftKnapsackProfile p;
  for (const auto& pr : items) {
    p.I_.push_back(pr.first);
    p.A_.push_back(pr.second);
  }
  const size_t m = p.I_.size();
  p.cumA_.assign(m + 1, 0.0);
  p.cumI_.assign(m + 1, 0.0);
  for (size_t j = 0; j < m; ++j) {
    p.cumA_[j + 1] = p.cumA_[j] + p.A_[j];
    p.cumI_[j + 1] = p.cumI_[j] + p.I_[j];
  }
  return p;
}

size_t nearest_node_manhattan(double px,
                              double py,
                              const UniformMesh& mesh) {
  size_t best = 0;
  double best_l1 = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < mesh.nodes.size(); ++i) {
    const auto& n = mesh.nodes[i];
    const double d = std::abs(px - n.x_um) + std::abs(py - n.y_um);
    if (d < best_l1) {
      best_l1 = d;
      best = i;
    }
  }
  return best;
}

// Search within a sub-range of a node vector.
size_t nearest_node_manhattan_range(double px,
                                   double py,
                                   const std::vector<MeshNode>& nodes,
                                   size_t lo,
                                   size_t hi) {
  size_t best = lo;
  double best_l1 = std::numeric_limits<double>::infinity();
  for (size_t i = lo; i < hi; ++i) {
    const auto& n = nodes[i];
    const double d = std::abs(px - n.x_um) + std::abs(py - n.y_um);
    if (d < best_l1) {
      best_l1 = d;
      best = i;
    }
  }
  return best;
}

size_t nearest_ring_node(size_t node_idx, const UniformMesh& mesh) {
  if (mesh.ring_nodes.empty())
    return std::numeric_limits<size_t>::max();
  const auto& src = mesh.nodes[node_idx];
  size_t best = mesh.ring_nodes.front();
  double best_l1 = std::numeric_limits<double>::infinity();
  for (size_t r : mesh.ring_nodes) {
    const auto& rn = mesh.nodes[r];
    const double d
        = std::abs(src.x_um - rn.x_um) + std::abs(src.y_um - rn.y_um);
    if (d < best_l1) {
      best_l1 = d;
      best = r;
    }
  }
  return best;
}

double inst_I(const Instance& inst, const EstOpts& opt) {
  if (inst.has_sta_power) {
    if (opt.prefer_sta_current && inst.sta_power.has_current
        && inst.sta_power.current_A > 0.0)
      return inst.sta_power.current_A;
    if (opt.vdd_V > 0.0 && inst.sta_power.total_W > 0.0)
      return inst.sta_power.total_W / opt.vdd_V;
  }
  if (inst.has_manual_power && opt.vdd_V > 0.0 && inst.manual_power_W > 0.0)
    return inst.manual_power_W / opt.vdd_V;
  return 0.0;
}

std::vector<double> unique_sorted(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end(),
                      [](double a, double b) {
                        return std::abs(a - b) < 1e-6;
                      }),
          v.end());
  return v;
}

// ---- legacy single-layer mesh helpers ----

UniformMesh make_mesh(const Layout& layout,
                      double pitch_x_um,
                      double pitch_y_um,
                      double v_init_V) {
  UniformMesh mesh;
  mesh.pitch_x_um = pitch_x_um;
  mesh.pitch_y_um = pitch_y_um;
  const double lx = layout.core[0];
  const double ly = layout.core[1];
  const double ux = layout.core[2];
  const double uy = layout.core[3];

  if (pitch_x_um <= 0.0 || pitch_y_um <= 0.0 || ux <= lx || uy <= ly)
    return mesh;

  mesh.nx = static_cast<int>(std::floor((ux - lx) / pitch_x_um)) + 1;
  mesh.ny = static_cast<int>(std::floor((uy - ly) / pitch_y_um)) + 1;
  mesh.nodes.reserve(static_cast<size_t>(mesh.nx * mesh.ny));
  for (int iy = 0; iy < mesh.ny; ++iy) {
    for (int ix = 0; ix < mesh.nx; ++ix) {
      MeshNode n;
      n.ix = ix;
      n.iy = iy;
      n.x_um = lx + ix * pitch_x_um;
      n.y_um = ly + iy * pitch_y_um;
      n.v_V = v_init_V;
      mesh.nodes.push_back(n);
    }
  }
  return mesh;
}

void mark_outer_ring(UniformMesh& mesh, double v_supply_V) {
  mesh.ring_nodes.clear();
  const int nx = mesh.nx;
  const int ny = mesh.ny;
  if (nx <= 0 || ny <= 0)
    return;
  for (size_t i = 0; i < mesh.nodes.size(); ++i) {
    auto& n = mesh.nodes[i];
    const bool on_ring
        = (n.ix == 0 || n.ix == nx - 1 || n.iy == 0 || n.iy == ny - 1);
    if (on_ring) {
      n.is_ring = true;
      n.v_src_V = v_supply_V;
      n.v_V = v_supply_V;
      mesh.ring_nodes.push_back(i);
    } else {
      n.is_ring = false;
      n.v_src_V = 0.0;
    }
  }
}

// Mark only the outer-ring nodes that coincide with a physical stripe position.
// vert_stripe_xs: sorted x-centers of vertical stripes; horiz_stripe_ys: sorted
// y-centers of horizontal stripes.  On the top/bottom ring edges, a node is a
// vsrc only if its x matches a vertical stripe.  On the left/right edges, only
// if its y matches a horizontal stripe.  Falls back to mark_outer_ring when
// both stripe sets are empty.
void mark_ring_at_stripe_crossings(UniformMesh& mesh,
                                   double v_supply_V,
                                   const std::vector<double>& vert_stripe_xs,
                                   const std::vector<double>& horiz_stripe_ys) {
  if (vert_stripe_xs.empty() && horiz_stripe_ys.empty()) {
    mark_outer_ring(mesh, v_supply_V);
    return;
  }
  const double tol = 0.5;
  auto matches_any = [&](double val, const std::vector<double>& sorted_set) {
    auto it = std::lower_bound(sorted_set.begin(), sorted_set.end(),
                               val - tol);
    return it != sorted_set.end() && *it <= val + tol;
  };

  mesh.ring_nodes.clear();
  const int nx = mesh.nx;
  const int ny = mesh.ny;
  if (nx <= 0 || ny <= 0)
    return;
  for (size_t i = 0; i < mesh.nodes.size(); ++i) {
    auto& n = mesh.nodes[i];
    n.is_ring = false;
    n.v_src_V = 0.0;
    const bool on_tb = (n.iy == 0 || n.iy == ny - 1);
    const bool on_lr = (n.ix == 0 || n.ix == nx - 1);
    if (!on_tb && !on_lr)
      continue;
    // Top/bottom edges: check against vertical stripe x-positions.
    // Left/right edges: check against horizontal stripe y-positions.
    bool at_crossing = false;
    if (on_tb && matches_any(n.x_um, vert_stripe_xs))
      at_crossing = true;
    if (on_lr && matches_any(n.y_um, horiz_stripe_ys))
      at_crossing = true;
    if (at_crossing) {
      n.is_ring = true;
      n.v_src_V = v_supply_V;
      n.v_V = v_supply_V;
      mesh.ring_nodes.push_back(i);
    }
  }
  if (mesh.ring_nodes.empty())
    mark_outer_ring(mesh, v_supply_V);
}

SoftIrData analyze_soft_modules(const Chip& chip, const EstOpts& opt) {
  struct Agg {
    std::string name;
    size_t n{};
    size_t n_std{};
    size_t n_macro{};
    double I_obs{};
    std::vector<std::pair<double, double>> items;
  };
  std::unordered_map<int, Agg> clusters;
  clusters.reserve(chip.instances.size() / 2);

  for (const auto& kv : chip.instances) {
    const auto& inst = kv.second;
    if (!inst.has_cluster)
      continue;
    const double I = inst_I(inst, opt);
    auto& g = clusters[inst.cluster_id];
    if (g.name.empty())
      g.name = inst.cluster_name.empty()
                   ? ("cluster_" + std::to_string(inst.cluster_id))
                   : inst.cluster_name;
    g.n++;
    if (inst.is_macro)
      g.n_macro++;
    else
      g.n_std++;
    g.I_obs += I;
    if (I > 0.0) {
      const double a = (inst.has_area && inst.area_um2 > 0.0)
                           ? inst.area_um2
                           : opt.assumed_cell_area_um2;
      g.items.push_back({I, std::max(a, std::numeric_limits<double>::min())});
    }
  }

  SoftIrData out;
  for (auto& kv : clusters) {
    const int cid = kv.first;
    auto& g = kv.second;
    if (g.n_std == 0)
      continue;

    ModuleCurrent m;
    m.cluster_id = cid;
    m.cluster_name = g.name;
    m.instance_count = g.n;
    m.observed_total_A = g.I_obs;

    if (const FpBox* b = find_fp(chip.fp, g.name))
      m.box_area_um2 = fp_area_um2(*b);

    SoftKnapsackProfile kn = make_profile_from_items(g.items);
    out.knapsack_by_name[m.cluster_name] = kn;

    const double cap_um2
        = m.box_area_um2 * std::max(0.0, opt.packing_utilization);
    double I_pack = kn.imax(cap_um2);
    if (cap_um2 <= 0.0)
      I_pack = m.observed_total_A;
    m.worst_case_A = std::max(I_pack, m.observed_total_A);
    m.i_mesh_sum_A = 0.0;
    out.modules.push_back(std::move(m));
  }

  std::sort(out.modules.begin(), out.modules.end(),
            [](const ModuleCurrent& a, const ModuleCurrent& b) {
              return a.worst_case_A > b.worst_case_A;
            });
  return out;
}

std::vector<HardMacroCurrent> hard_currents(const Chip& chip,
                                            const EstOpts& opt) {
  std::vector<HardMacroCurrent> out;
  for (const auto& box : chip.fp) {
    if (!fp_is_hard(chip, box))
      continue;
    const auto toks = fp_tokens(box.name);
    if (toks.empty())
      continue;

    for (const auto& t : toks) {
      auto it = chip.instances.find(t);
      if (it == chip.instances.end()) {
        throw std::runtime_error(
            "hard_currents: instance \"" + t
            + "\" not in Chip::instances (required for hard-macro PG pins).");
      }
      if (!it->second.has_pg_pin || it->second.pg_pins.empty()) {
        throw std::runtime_error(
            "hard_currents: hard macro \"" + t
            + "\" has no ODB PG geometry (instance_geom.tsv). LEF/ODB must "
              "expose POWER/GROUND pins.");
      }
      const auto& inst = it->second;
      const double I_tot = inst_I(inst, opt);
      const double n_pin = static_cast<double>(inst.pg_pins.size());
      const double I_each = I_tot / n_pin;
      for (const auto& pin : inst.pg_pins) {
        HardMacroCurrent h;
        h.instance = t;
        h.fp_region = box.name;
        h.pg_pin_name = pin.name;
        h.current_A = I_each;
        h.pin_x_um = pin.x_um;
        h.pin_y_um = pin.y_um;
        out.push_back(std::move(h));
      }
    }
  }
  std::sort(out.begin(), out.end(),
            [](const HardMacroCurrent& a, const HardMacroCurrent& b) {
              return a.current_A > b.current_A;
            });
  return out;
}

// Legacy single-layer load assignment.
void assign_mesh_loads_single(const Chip& chip,
                              const Layout& layout,
                              UniformMesh& mesh,
                              const std::vector<HardMacroCurrent>& hard,
                              SoftIrData& soft_data) {
  for (auto& n : mesh.nodes) {
    n.I_soft_A = 0.0;
    n.I_hard_A = 0.0;
  }

  const double core_lx = layout.core[0];
  const double core_ly = layout.core[1];
  const double core_ux = layout.core[2];
  const double core_uy = layout.core[3];
  const double mesh_lx = core_lx;
  const double mesh_ly = core_ly;
  const int nx = mesh.nx;
  const int ny = mesh.ny;
  const double px = mesh.pitch_x_um;
  const double py = mesh.pitch_y_um;

  std::unordered_map<std::string, double> sum_imax_by_name;
  sum_imax_by_name.reserve(soft_data.modules.size());

  for (const auto& mod : soft_data.modules) {
    auto kit = soft_data.knapsack_by_name.find(mod.cluster_name);
    if (kit == soft_data.knapsack_by_name.end())
      continue;
    const FpBox* box = find_fp(chip.fp, mod.cluster_name);
    if (!box)
      continue;
    const SoftKnapsackProfile& kn = kit->second;
    const double bx0 = box->lx;
    const double by0 = box->ly;
    const double bx1 = box->ux;
    const double by1 = box->uy;

    for (int iy = 0; iy < ny; ++iy) {
      for (int ix = 0; ix < nx; ++ix) {
        double tx0, ty0, tx1, ty1;
        node_tile_um(ix, iy, nx, ny, core_lx, core_ly, core_ux, core_uy, px,
                     py, mesh_lx, mesh_ly, &tx0, &ty0, &tx1, &ty1);
        double ix0, iy0, ix1, iy1;
        const double A_ov = rect_overlap_um2(tx0, ty0, tx1, ty1, bx0, by0,
                                             bx1, by1, &ix0, &iy0, &ix1, &iy1);
        if (A_ov <= 0.0)
          continue;
        const double I_add = kn.imax(A_ov);
        if (I_add <= 0.0)
          continue;
        const size_t idx = static_cast<size_t>(iy * nx + ix);
        mesh.nodes[idx].I_soft_A += I_add;
        sum_imax_by_name[mod.cluster_name] += I_add;
      }
    }
  }

  for (auto& m : soft_data.modules) {
    auto it = sum_imax_by_name.find(m.cluster_name);
    m.i_mesh_sum_A = (it != sum_imax_by_name.end()) ? it->second : 0.0;
  }

  for (const auto& h : hard) {
    const size_t j = nearest_node_manhattan(h.pin_x_um, h.pin_y_um, mesh);
    mesh.nodes[j].I_hard_A += h.current_A;
  }
}

double node_voltage_under_load(const UniformMesh& mesh,
                               const StrapSheetModel& strap,
                               size_t node_idx,
                               double I_A,
                               IrMode mode) {
  if (mesh.ring_nodes.empty())
    return std::numeric_limits<double>::quiet_NaN();
  const auto& load = mesh.nodes[node_idx];
  const size_t r = nearest_ring_node(node_idx, mesh);
  if (r == std::numeric_limits<size_t>::max())
    return std::numeric_limits<double>::quiet_NaN();
  const auto& ring = mesh.nodes[r];
  const double dx = std::abs(load.x_um - ring.x_um);
  const double dy = std::abs(load.y_um - ring.y_um);
  return ring.v_src_V - ir_drop(strap, dx, dy, I_A, mode);
}

double local_max_drop(const StrapSheetModel& strap,
                      double dx_um,
                      double dy_um,
                      double I_A) {
  double rh = 0.0;
  double rv = 0.0;
  if (strap.w_hstrap_um > 0.0)
    rh = strap.r_sqh * dx_um / strap.w_hstrap_um;
  if (strap.w_vstrap_um > 0.0)
    rv = strap.r_sqv * dy_um / strap.w_vstrap_um;
  return I_A * std::max(rh, rv);
}

// Layer-specific local drop: R = r_per_um * dist / width along the strap's
// preferred axis (horizontal -> x, vertical -> y).
double local_layer_drop(const LayerInfo& li,
                        double dx_um,
                        double dy_um,
                        double I_A) {
  if (li.width_um <= 0.0 || li.r_per_um <= 0.0)
    return 0.0;
  const double dist = li.is_horizontal ? dx_um : dy_um;
  return I_A * li.r_per_um * dist / li.width_um;
}

double soft_tile_worst_V(const UniformMesh& mesh,
                         const StrapSheetModel& strap,
                         const FpBox& module_box,
                         const SoftKnapsackProfile& kn,
                         const Layout& layout,
                         IrMode mode) {
  if (mesh.nodes.empty() || mesh.ring_nodes.empty())
    return std::numeric_limits<double>::quiet_NaN();

  const double core_lx = layout.core[0];
  const double core_ly = layout.core[1];
  const double core_ux = layout.core[2];
  const double core_uy = layout.core[3];
  const double mesh_lx = core_lx;
  const double mesh_ly = core_ly;
  const int nx = mesh.nx;
  const int ny = mesh.ny;
  const double px = mesh.pitch_x_um;
  const double py = mesh.pitch_y_um;

  const double bx0 = module_box.lx;
  const double by0 = module_box.ly;
  const double bx1 = module_box.ux;
  const double by1 = module_box.uy;

  double vmin = std::numeric_limits<double>::infinity();
  for (int iy = 0; iy < ny; ++iy) {
    for (int ix = 0; ix < nx; ++ix) {
      double tx0, ty0, tx1, ty1;
      node_tile_um(ix, iy, nx, ny, core_lx, core_ly, core_ux, core_uy, px, py,
                   mesh_lx, mesh_ly, &tx0, &ty0, &tx1, &ty1);
      double ox0, oy0, ox1, oy1;
      const double A_ov = rect_overlap_um2(tx0, ty0, tx1, ty1, bx0, by0, bx1,
                                           by1, &ox0, &oy0, &ox1, &oy1);
      if (A_ov <= 0.0)
        continue;
      const double I_nk = kn.imax(A_ov);
      if (I_nk <= 0.0)
        continue;
      const size_t idx = static_cast<size_t>(iy * nx + ix);
      double v;
      if (mode == IrMode::Sum) {
        v = node_voltage_under_load(mesh, strap, idx, I_nk, mode);
      } else {
        const auto& ni = mesh.nodes[idx];
        const double dx = std::max(std::abs(ox1 - ni.x_um),
                                   std::abs(ox0 - ni.x_um));
        const double dy = std::max(std::abs(oy1 - ni.y_um),
                                   std::abs(oy0 - ni.y_um));
        v = ni.v_V - local_max_drop(strap, dx, dy, I_nk);
      }
      if (std::isfinite(v))
        vmin = std::min(vmin, v);
    }
  }
  if (!std::isfinite(vmin))
    return std::numeric_limits<double>::quiet_NaN();
  return vmin;
}

double hard_pin_V(const UniformMesh& mesh,
                  const StrapSheetModel& strap,
                  double pin_x_um,
                  double pin_y_um,
                  double I_A,
                  IrMode mode) {
  if (mesh.nodes.empty() || mesh.ring_nodes.empty())
    return std::numeric_limits<double>::quiet_NaN();
  const size_t j = nearest_node_manhattan(pin_x_um, pin_y_um, mesh);
  if (mode == IrMode::Sum)
    return node_voltage_under_load(mesh, strap, j, I_A, mode);
  const auto& ni = mesh.nodes[j];
  const double dx = std::abs(pin_x_um - ni.x_um);
  const double dy = std::abs(pin_y_um - ni.y_um);
  return ni.v_V - local_max_drop(strap, dx, dy, I_A);
}

// ---- legacy single-layer ODB mesh ----

UniformMesh build_mesh_from_pdn_dump(
    const Layout& layout,
    const PdnDump& dump,
    const std::unordered_map<std::string, double>& rc_r_per_um,
    double v_supply_V,
    StrapSheetModel* strap_out) {
  UniformMesh mesh;
  const double lx = layout.core[0];
  const double ly = layout.core[1];
  const double ux = layout.core[2];
  const double uy = layout.core[3];
  if (ux <= lx || uy <= ly)
    return mesh;

  std::vector<double> xs;
  std::vector<double> ys;
  std::vector<double> stripe_xs;  // x-centers of vertical STRIPEs only
  std::vector<double> stripe_ys;  // y-centers of horizontal STRIPEs only
  double w_h_sum = 0.0;
  int w_h_cnt = 0;
  double w_v_sum = 0.0;
  int w_v_cnt = 0;
  std::string layer_h;
  std::string layer_v;

  std::vector<PdnSegment> rings;

  for (const auto& s : dump.segs) {
    const double w = s.xhi - s.xlo;
    const double h = s.yhi - s.ylo;
    if (w <= 0.0 || h <= 0.0)
      continue;
    const bool vertical = w < h;
    const double cx = 0.5 * (s.xlo + s.xhi);
    const double cy = 0.5 * (s.ylo + s.yhi);
    if (s.kind == "RING") {
      rings.push_back(s);
      if (vertical)
        xs.push_back(cx);
      else
        ys.push_back(cy);
      continue;
    }
    if (s.kind != "STRIPE")
      continue;
    if (vertical) {
      xs.push_back(cx);
      stripe_xs.push_back(cx);
      w_v_sum += w;
      ++w_v_cnt;
      if (layer_v.empty())
        layer_v = s.layer;
    } else {
      ys.push_back(cy);
      stripe_ys.push_back(cy);
      w_h_sum += h;
      ++w_h_cnt;
      if (layer_h.empty())
        layer_h = s.layer;
    }
  }
  stripe_xs = unique_sorted(std::move(stripe_xs));
  stripe_ys = unique_sorted(std::move(stripe_ys));

  xs.push_back(lx);
  xs.push_back(ux);
  ys.push_back(ly);
  ys.push_back(uy);

  xs = unique_sorted(std::move(xs));
  ys = unique_sorted(std::move(ys));

  xs.erase(std::remove_if(xs.begin(), xs.end(),
                          [&](double v) { return v < lx || v > ux; }),
           xs.end());
  ys.erase(std::remove_if(ys.begin(), ys.end(),
                          [&](double v) { return v < ly || v > uy; }),
           ys.end());

  if (xs.size() < 2 || ys.size() < 2)
    return mesh;

  mesh.nx = static_cast<int>(xs.size());
  mesh.ny = static_cast<int>(ys.size());
  auto median_diff = [](const std::vector<double>& v) {
    if (v.size() < 2)
      return 0.0;
    std::vector<double> d;
    d.reserve(v.size() - 1);
    for (size_t i = 1; i < v.size(); ++i)
      d.push_back(v[i] - v[i - 1]);
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
  };
  mesh.pitch_x_um = median_diff(xs);
  mesh.pitch_y_um = median_diff(ys);

  mesh.nodes.reserve(static_cast<size_t>(mesh.nx * mesh.ny));
  for (int iy = 0; iy < mesh.ny; ++iy) {
    for (int ix = 0; ix < mesh.nx; ++ix) {
      MeshNode n;
      n.ix = ix;
      n.iy = iy;
      n.x_um = xs[static_cast<size_t>(ix)];
      n.y_um = ys[static_cast<size_t>(iy)];
      n.v_V = v_supply_V;
      mesh.nodes.push_back(n);
    }
  }

  const double slack = 0.5;
  auto inside = [&](const PdnSegment& s, double x, double y) {
    return x >= s.xlo - slack && x <= s.xhi + slack
           && y >= s.ylo - slack && y <= s.yhi + slack;
  };
  auto matches_any = [&](double val, const std::vector<double>& sorted_set) {
    auto it = std::lower_bound(sorted_set.begin(), sorted_set.end(),
                               val - slack);
    return it != sorted_set.end() && *it <= val + slack;
  };
  // Only mark ring nodes at stripe-ring intersections: a node on a horizontal
  // ring must also sit on a vertical stripe x-center, and vice versa.
  mesh.ring_nodes.clear();
  for (size_t i = 0; i < mesh.nodes.size(); ++i) {
    auto& n = mesh.nodes[i];
    bool at_intersection = false;
    for (const auto& r : rings) {
      if (!inside(r, n.x_um, n.y_um))
        continue;
      const bool ring_horiz = (r.xhi - r.xlo) > (r.yhi - r.ylo);
      if (ring_horiz)
        at_intersection = matches_any(n.x_um, stripe_xs);
      else
        at_intersection = matches_any(n.y_um, stripe_ys);
      if (at_intersection)
        break;
    }
    if (at_intersection) {
      n.is_ring = true;
      n.v_src_V = v_supply_V;
      n.v_V = v_supply_V;
      mesh.ring_nodes.push_back(i);
    }
  }

  if (mesh.ring_nodes.empty())
    mark_outer_ring(mesh, v_supply_V);

  if (strap_out) {
    StrapSheetModel strap;
    strap.w_hstrap_um = (w_h_cnt > 0) ? (w_h_sum / w_h_cnt) : 0.0;
    strap.w_vstrap_um = (w_v_cnt > 0) ? (w_v_sum / w_v_cnt) : 0.0;
    auto lookup_r = [&](const std::string& layer) {
      auto it = rc_r_per_um.find(layer);
      return (it == rc_r_per_um.end()) ? 0.0 : it->second;
    };
    const double rh_per_um = lookup_r(layer_h);
    const double rv_per_um = lookup_r(layer_v);
    strap.r_sqh = rh_per_um * strap.w_hstrap_um;
    strap.r_sqv = rv_per_um * strap.w_vstrap_um;
    *strap_out = strap;
  }

  return mesh;
}

// ---- legacy single-layer CG solver (5-point stencil) ----

void matvec_G(const UniformMesh& mesh,
              const StrapSheetModel& strap,
              const std::vector<double>& x,
              std::vector<double>& Gx) {
  const size_t N = mesh.nodes.size();
  const int nx = mesh.nx;
  Gx.assign(N, 0.0);
  for (size_t idx = 0; idx < N; ++idx) {
    const auto& n = mesh.nodes[idx];
    if (n.is_ring) {
      Gx[idx] = x[idx];
      continue;
    }
    const int ix = n.ix;
    const int iy = n.iy;
    double off_sum = 0.0;
    double diag = 0.0;
    if (ix > 0) {
      const size_t j = idx - 1;
      const double dx = n.x_um - mesh.nodes[j].x_um;
      if (dx > 0.0 && strap.r_sqh > 0.0) {
        const double g = strap.w_hstrap_um / (strap.r_sqh * dx);
        diag += g;
        off_sum -= g * x[j];
      }
    }
    if (ix < nx - 1) {
      const size_t j = idx + 1;
      const double dx = mesh.nodes[j].x_um - n.x_um;
      if (dx > 0.0 && strap.r_sqh > 0.0) {
        const double g = strap.w_hstrap_um / (strap.r_sqh * dx);
        diag += g;
        off_sum -= g * x[j];
      }
    }
    if (iy > 0) {
      const size_t j = idx - static_cast<size_t>(nx);
      const double dy = n.y_um - mesh.nodes[j].y_um;
      if (dy > 0.0 && strap.r_sqv > 0.0) {
        const double g = strap.w_vstrap_um / (strap.r_sqv * dy);
        diag += g;
        off_sum -= g * x[j];
      }
    }
    if (iy < mesh.ny - 1) {
      const size_t j = idx + static_cast<size_t>(nx);
      const double dy = mesh.nodes[j].y_um - n.y_um;
      if (dy > 0.0 && strap.r_sqv > 0.0) {
        const double g = strap.w_vstrap_um / (strap.r_sqv * dy);
        diag += g;
        off_sum -= g * x[j];
      }
    }
    Gx[idx] = diag * x[idx] + off_sum;
  }
}

double diag_G(size_t idx,
              const UniformMesh& mesh,
              const StrapSheetModel& strap) {
  const auto& n = mesh.nodes[idx];
  if (n.is_ring)
    return 1.0;
  const int nx = mesh.nx;
  const int ix = n.ix;
  const int iy = n.iy;
  double d = 0.0;
  if (ix > 0) {
    const double dx = n.x_um - mesh.nodes[idx - 1].x_um;
    if (dx > 0.0 && strap.r_sqh > 0.0)
      d += strap.w_hstrap_um / (strap.r_sqh * dx);
  }
  if (ix < nx - 1) {
    const double dx = mesh.nodes[idx + 1].x_um - n.x_um;
    if (dx > 0.0 && strap.r_sqh > 0.0)
      d += strap.w_hstrap_um / (strap.r_sqh * dx);
  }
  if (iy > 0) {
    const double dy = n.y_um - mesh.nodes[idx - static_cast<size_t>(nx)].y_um;
    if (dy > 0.0 && strap.r_sqv > 0.0)
      d += strap.w_vstrap_um / (strap.r_sqv * dy);
  }
  if (iy < mesh.ny - 1) {
    const double dy = mesh.nodes[idx + static_cast<size_t>(nx)].y_um - n.y_um;
    if (dy > 0.0 && strap.r_sqv > 0.0)
      d += strap.w_vstrap_um / (strap.r_sqv * dy);
  }
  return d;
}

void solve_mesh_cg(UniformMesh& mesh,
                   const StrapSheetModel& strap,
                   double tol = 1e-10,
                   int max_iter = 5000) {
  const size_t N = mesh.nodes.size();
  if (N == 0)
    return;

  std::vector<double> b(N);
  std::vector<double> x(N);
  std::vector<double> diag_inv(N);
  for (size_t i = 0; i < N; ++i) {
    const auto& n = mesh.nodes[i];
    if (n.is_ring) {
      b[i] = n.v_src_V;
      x[i] = n.v_src_V;
    } else {
      b[i] = -(n.I_soft_A + n.I_hard_A);
      x[i] = n.v_V;
    }
    const double d = diag_G(i, mesh, strap);
    diag_inv[i] = (d > 0.0) ? (1.0 / d) : 1.0;
  }

  std::vector<double> Gx(N);
  matvec_G(mesh, strap, x, Gx);
  std::vector<double> r(N);
  for (size_t i = 0; i < N; ++i)
    r[i] = b[i] - Gx[i];

  std::vector<double> z(N);
  double rz = 0.0;
  for (size_t i = 0; i < N; ++i) {
    z[i] = r[i] * diag_inv[i];
    rz += r[i] * z[i];
  }

  std::vector<double> p = z;
  std::vector<double> Ap(N);

  for (int iter = 0; iter < max_iter; ++iter) {
    double r_norm2 = 0.0;
    for (size_t i = 0; i < N; ++i)
      r_norm2 += r[i] * r[i];
    if (r_norm2 < tol * tol)
      break;

    matvec_G(mesh, strap, p, Ap);

    double pAp = 0.0;
    for (size_t i = 0; i < N; ++i)
      pAp += p[i] * Ap[i];
    if (pAp <= 0.0)
      break;

    const double alpha = rz / pAp;
    for (size_t i = 0; i < N; ++i) {
      x[i] += alpha * p[i];
      r[i] -= alpha * Ap[i];
    }

    double rz_new = 0.0;
    for (size_t i = 0; i < N; ++i) {
      z[i] = r[i] * diag_inv[i];
      rz_new += r[i] * z[i];
    }

    if (std::abs(rz) < 1e-30)
      break;
    const double beta = rz_new / rz;
    for (size_t i = 0; i < N; ++i)
      p[i] = z[i] + beta * p[i];
    rz = rz_new;
  }

  for (size_t i = 0; i < N; ++i)
    mesh.nodes[i].v_V = x[i];
}

// =====================================================================
// ---- Multi-layer mesh construction and solver ----
// =====================================================================

// Build a multi-layer mesh from PDN dump following grid_strategy hierarchy.
// Layer ordering: bottom (M1 followpins) ¡÷ middle (M4 vertical straps) ¡÷ top
// (M7 horizontal straps).  Inter-layer via connections follow pdn_connects.
MultiLayerMesh build_multilayer_mesh(
    const Layout& layout,
    const Tech& tech,
    const PdnDump& dump,
    const std::unordered_map<std::string, double>& rc_r_per_um,
    double v_supply_V,
    double via_r_ohm) {
  MultiLayerMesh ml;
  const double lx = layout.core[0];
  const double ly = layout.core[1];
  const double ux = layout.core[2];
  const double uy = layout.core[3];
  if (ux <= lx || uy <= ly)
    return ml;

  // 1. Identify core grid layers from grid_strategy stripes (grid == "grid").
  struct CoreLayer {
    std::string name;
    double width{};
    double pitch{};
    bool followpins{};
  };
  std::vector<CoreLayer> core_layers;
  for (const auto& s : tech.stripes) {
    if (!s.grid.empty() && s.grid != "grid")
      continue;
    if (s.width <= 0.0 || s.pitch <= 0.0)
      continue;
    bool dup = false;
    for (const auto& c : core_layers) {
      if (c.name == s.layer) {
        dup = true;
        break;
      }
    }
    if (!dup)
      core_layers.push_back({s.layer, s.width, s.pitch, s.followpins});
  }
  if (core_layers.empty())
    return ml;

  // Determine pin layer from define_pdn_grid -pins.
  for (const auto& g : tech.pdn_grids) {
    if (g.name == "grid" && !g.pins.empty()) {
      ml.pin_layer = g.pins.front();
      break;
    }
  }

  // 2. Classify each layer's segments from dump and determine orientation.
  struct SegBucket {
    std::string kind;  // "STRIPE" or "FOLLOWPIN"
    std::vector<double> centers;  // center coordinate along the thin dimension
    double w_sum{};
    int w_cnt{};
    int vert_cnt{};
    int horiz_cnt{};
  };
  std::unordered_map<std::string, SegBucket> buckets;
  std::vector<PdnSegment> rings;
  std::vector<PdnSegment> bterms;

  for (const auto& s : dump.segs) {
    if (s.kind == "RING") {
      rings.push_back(s);
      continue;
    }
    if (s.kind == "BTERM") {
      bterms.push_back(s);
      continue;
    }
    const double w = s.xhi - s.xlo;
    const double h = s.yhi - s.ylo;
    if (w <= 0.0 || h <= 0.0)
      continue;
    const bool vertical = w < h;
    auto& bk = buckets[s.layer];
    if (bk.kind.empty())
      bk.kind = s.kind;
    if (vertical) {
      bk.centers.push_back(0.5 * (s.xlo + s.xhi));
      bk.w_sum += w;
      bk.w_cnt++;
      bk.vert_cnt++;
    } else {
      bk.centers.push_back(0.5 * (s.ylo + s.yhi));
      bk.w_sum += h;
      bk.w_cnt++;
      bk.horiz_cnt++;
    }
  }

  // 3. Build LayerInfo for each core grid layer (sorted bottom ¡÷ top).
  //    Orientation is determined from the actual dump geometry.
  std::vector<LayerInfo> layer_infos;
  for (const auto& cl : core_layers) {
    LayerInfo li;
    li.name = cl.name;
    li.width_um = cl.width;
    li.pitch_um = cl.pitch;
    li.is_followpin = cl.followpins;
    auto rit = rc_r_per_um.find(cl.name);
    li.r_per_um = (rit != rc_r_per_um.end()) ? rit->second : 0.0;

    auto bit = buckets.find(cl.name);
    if (bit != buckets.end()) {
      li.is_horizontal = (bit->second.horiz_cnt >= bit->second.vert_cnt);
    } else {
      li.is_horizontal = cl.followpins;
    }
    layer_infos.push_back(std::move(li));
  }
  if (layer_infos.empty())
    return ml;

  // Assign layer indices (bottom to top: followpin first, then by
  // ascending metal number extracted from name, e.g. "metal1" < "metal4").
  auto metal_num = [](const std::string& s) -> int {
    size_t p = s.find_first_of("0123456789");
    if (p == std::string::npos)
      return 999;
    return std::stoi(s.substr(p));
  };
  std::sort(layer_infos.begin(), layer_infos.end(),
            [&](const LayerInfo& a, const LayerInfo& b) {
              if (a.is_followpin != b.is_followpin)
                return a.is_followpin;
              return metal_num(a.name) < metal_num(b.name);
            });
  for (size_t i = 0; i < layer_infos.size(); ++i)
    layer_infos[i].idx = static_cast<int>(i);

  // soft = bottom layer (followpin / M1); hard = next strap layer (M4).
  ml.soft_layer_idx = 0;
  ml.hard_layer_idx = (layer_infos.size() > 1) ? 1 : 0;

  // 4. Extract per-layer coordinate sets from the dump.
  //    Vertical straps contribute x-centers; horizontal straps contribute
  //    y-centers. All layers share the x-coordinates of the vertical strap
  //    layer so that via intersections are aligned.

  // Identify the vertical strap layer to get shared x-coordinates.
  std::vector<double> shared_xs;
  for (const auto& li : layer_infos) {
    if (li.is_horizontal)
      continue;
    auto bit = buckets.find(li.name);
    if (bit == buckets.end())
      continue;
    for (double c : bit->second.centers)
      shared_xs.push_back(c);
  }
  shared_xs = unique_sorted(std::move(shared_xs));
  // Clip to core.
  shared_xs.erase(
      std::remove_if(shared_xs.begin(), shared_xs.end(),
                     [&](double v) { return v < lx - 0.01 || v > ux + 0.01; }),
      shared_xs.end());
  if (shared_xs.empty())
    return ml;

  // Per-layer y-coordinates.
  for (auto& li : layer_infos) {
    li.x_coords = shared_xs;
    li.nx = static_cast<int>(shared_xs.size());
    auto bit = buckets.find(li.name);
    if (bit == buckets.end()) {
      li.y_coords = {};
      li.ny = 0;
      continue;
    }
    if (li.is_horizontal) {
      std::vector<double> ys = unique_sorted(bit->second.centers);
      ys.erase(std::remove_if(ys.begin(), ys.end(),
                               [&](double v) {
                                 return v < ly - 0.01 || v > uy + 0.01;
                               }),
               ys.end());
      li.y_coords = std::move(ys);
    } else {
      // Vertical layer (M4): y-coords = union of all horizontal layers' y-coords.
      // Will be filled after all layers are processed.
      li.y_coords = {};
    }
    li.ny = static_cast<int>(li.y_coords.size());
  }

  // For vertical layer(s), merge all horizontal layers' y-coords.
  for (auto& li : layer_infos) {
    if (li.is_horizontal)
      continue;
    std::vector<double> all_ys;
    for (const auto& other : layer_infos) {
      if (&other == &li)
        continue;
      if (!other.is_horizontal)
        continue;
      for (double y : other.y_coords)
        all_ys.push_back(y);
    }
    li.y_coords = unique_sorted(std::move(all_ys));
    li.ny = static_cast<int>(li.y_coords.size());
  }

  // 5. Create nodes for each layer.
  ml.layers = layer_infos;
  ml.layer_node_offset.resize(layer_infos.size() + 1);
  size_t total_nodes = 0;
  for (size_t li_idx = 0; li_idx < layer_infos.size(); ++li_idx) {
    ml.layer_node_offset[li_idx] = total_nodes;
    const auto& li = layer_infos[li_idx];
    total_nodes += static_cast<size_t>(li.nx) * static_cast<size_t>(li.ny);
  }
  ml.layer_node_offset[layer_infos.size()] = total_nodes;

  ml.nodes.reserve(total_nodes);
  for (size_t li_idx = 0; li_idx < layer_infos.size(); ++li_idx) {
    const auto& li = layer_infos[li_idx];
    for (int iy = 0; iy < li.ny; ++iy) {
      for (int ix = 0; ix < li.nx; ++ix) {
        MeshNode n;
        n.ix = ix;
        n.iy = iy;
        n.x_um = li.x_coords[static_cast<size_t>(ix)];
        n.y_um = li.y_coords[static_cast<size_t>(iy)];
        n.v_V = v_supply_V;
        n.layer_idx = static_cast<int>(li_idx);
        n.layer_name = li.name;
        ml.nodes.push_back(n);
      }
    }
  }

  // 6. Build adjacency list.
  ml.adj.resize(total_nodes);

  // Helper: conductance for a strap segment of length dist on a given layer.
  auto strap_g = [](const LayerInfo& li, double dist) -> double {
    if (dist <= 0.0 || li.r_per_um <= 0.0 || li.width_um <= 0.0)
      return 0.0;
    return li.width_um / (li.r_per_um * dist);
  };

  // Via conductance.
  const double g_via = (via_r_ohm > 0.0) ? (1.0 / via_r_ohm) : 1e12;

  // Same-layer connections.
  for (size_t li_idx = 0; li_idx < layer_infos.size(); ++li_idx) {
    const auto& li = layer_infos[li_idx];
    const size_t off = ml.layer_node_offset[li_idx];
    for (int iy = 0; iy < li.ny; ++iy) {
      for (int ix = 0; ix < li.nx; ++ix) {
        const size_t idx = off + static_cast<size_t>(iy * li.nx + ix);
        if (li.is_horizontal) {
          // Horizontal connections (left/right neighbors on same y).
          if (ix > 0) {
            const size_t j = idx - 1;
            const double dx = ml.nodes[idx].x_um - ml.nodes[j].x_um;
            const double g = strap_g(li, dx);
            if (g > 0.0) {
              ml.adj[idx].push_back({j, g});
              ml.adj[j].push_back({idx, g});
            }
          }
        } else {
          // Vertical connections (up/down neighbors on same x).
          if (iy > 0) {
            const size_t j = idx - static_cast<size_t>(li.nx);
            const double dy = ml.nodes[idx].y_um - ml.nodes[j].y_um;
            const double g = strap_g(li, dy);
            if (g > 0.0) {
              ml.adj[idx].push_back({j, g});
              ml.adj[j].push_back({idx, g});
            }
          }
        }
      }
    }
  }

  // Inter-layer via connections: based on pdn_connects for the core grid.
  // For each connect pair, find nodes at matching (x, y) coordinates.
  auto find_layer_idx = [&](const std::string& name) -> int {
    for (size_t i = 0; i < layer_infos.size(); ++i) {
      if (layer_infos[i].name == name)
        return static_cast<int>(i);
    }
    return -1;
  };

  // Build spatial lookup for each layer: (x, y) -> node index.
  // Key: quantized (x*1e4 + y*1e-4) is imprecise; use a map from (ix_in_shared, iy_in_layer).
  // For via connections between layers that share x-coords, we match by
  // ix (same position in shared_xs) and by finding the closest y.
  for (const auto& conn : tech.pdn_connects) {
    if (!conn.grid.empty() && conn.grid != "grid")
      continue;
    const int la_idx = find_layer_idx(conn.layer_lower);
    const int lb_idx = find_layer_idx(conn.layer_upper);
    if (la_idx < 0 || lb_idx < 0)
      continue;
    const auto& la = layer_infos[static_cast<size_t>(la_idx)];
    const auto& lb = layer_infos[static_cast<size_t>(lb_idx)];
    const size_t off_a = ml.layer_node_offset[static_cast<size_t>(la_idx)];
    const size_t off_b = ml.layer_node_offset[static_cast<size_t>(lb_idx)];

    // Both layers share the same x_coords (shared_xs), so iterate by ix.
    const int nx_shared = std::min(la.nx, lb.nx);
    for (int ix = 0; ix < nx_shared; ++ix) {
      // For each y in the smaller layer, find matching y in the larger.
      // The vertical layer (M4) has y = union(M1_y, M7_y), so all M1/M7 y
      // values exist in M4.
      const auto& ya = la.y_coords;
      const auto& yb = lb.y_coords;
      size_t jb = 0;
      for (int iya = 0; iya < static_cast<int>(ya.size()); ++iya) {
        const double target_y = ya[static_cast<size_t>(iya)];
        // Advance jb to the closest match in yb.
        while (jb + 1 < yb.size()
               && std::abs(yb[jb + 1] - target_y) < std::abs(yb[jb] - target_y))
          ++jb;
        if (jb >= yb.size())
          break;
        if (std::abs(yb[jb] - target_y) > 0.01)
          continue;  // no match within tolerance
        const size_t idx_a = off_a + static_cast<size_t>(iya * la.nx + ix);
        const size_t idx_b = off_b + static_cast<size_t>(static_cast<int>(jb) * lb.nx + ix);
        ml.adj[idx_a].push_back({idx_b, g_via});
        ml.adj[idx_b].push_back({idx_a, g_via});
      }
    }
  }

  // 7. Mark voltage sources.
  ml.ring_nodes.clear();
  const double slack = 0.5;

  // Priority 1: RING segments.
  if (!rings.empty()) {
    for (size_t i = 0; i < ml.nodes.size(); ++i) {
      auto& n = ml.nodes[i];
      for (const auto& r : rings) {
        if (n.x_um >= r.xlo - slack && n.x_um <= r.xhi + slack
            && n.y_um >= r.ylo - slack && n.y_um <= r.yhi + slack) {
          n.is_ring = true;
          n.v_src_V = v_supply_V;
          n.v_V = v_supply_V;
          ml.ring_nodes.push_back(i);
          break;
        }
      }
    }
  }

  // Priority 2: BTERM segments on pin layer (e.g. M7).
  // Only mark nodes where the BTERM stripe meets the core boundary (die edge),
  // not along the entire BTERM length.
  if (ml.ring_nodes.empty() && !bterms.empty() && !ml.pin_layer.empty()) {
    const int pin_li = find_layer_idx(ml.pin_layer);
    if (pin_li >= 0) {
      const size_t pin_off = ml.layer_node_offset[static_cast<size_t>(pin_li)];
      const size_t pin_end = ml.layer_node_offset[static_cast<size_t>(pin_li + 1)];
      const double bnd_tol = 2.0;  // tolerance for "BTERM reaches die edge"
      for (const auto& bt : bterms) {
        if (bt.layer != ml.pin_layer)
          continue;
        const bool horiz = (bt.xhi - bt.xlo) > (bt.yhi - bt.ylo);
        const bool reaches_lo = horiz ? (bt.xlo <= lx + bnd_tol)
                                      : (bt.ylo <= ly + bnd_tol);
        const bool reaches_hi = horiz ? (bt.xhi >= ux - bnd_tol)
                                      : (bt.yhi >= uy - bnd_tol);
        if (!reaches_lo && !reaches_hi)
          continue;
        // Collect nodes that fall within this BTERM.
        struct Hit {
          size_t idx;
          double along;  // coordinate along the stripe direction
        };
        std::vector<Hit> hits;
        for (size_t i = pin_off; i < pin_end; ++i) {
          const auto& n = ml.nodes[i];
          if (n.x_um >= bt.xlo - slack && n.x_um <= bt.xhi + slack
              && n.y_um >= bt.ylo - slack && n.y_um <= bt.yhi + slack) {
            hits.push_back({i, horiz ? n.x_um : n.y_um});
          }
        }
        if (hits.empty())
          continue;
        // Find extremes along stripe direction.
        double lo_val = hits.front().along, hi_val = hits.front().along;
        for (const auto& h : hits) {
          lo_val = std::min(lo_val, h.along);
          hi_val = std::max(hi_val, h.along);
        }
        // Mark only the node(s) at the die-boundary endpoints.
        for (const auto& h : hits) {
          const bool at_lo = reaches_lo && std::abs(h.along - lo_val) < 0.01;
          const bool at_hi = reaches_hi && std::abs(h.along - hi_val) < 0.01;
          if (at_lo || at_hi) {
            auto& n = ml.nodes[h.idx];
            if (!n.is_ring) {
              n.is_ring = true;
              n.v_src_V = v_supply_V;
              n.v_V = v_supply_V;
              ml.ring_nodes.push_back(h.idx);
            }
          }
        }
      }
    }
  }

  // Priority 3: Fallback ¡X mark outermost nodes on the top (pin) layer.
  if (ml.ring_nodes.empty()) {
    int top_li = find_layer_idx(ml.pin_layer);
    if (top_li < 0)
      top_li = static_cast<int>(layer_infos.size()) - 1;
    const auto& tl = layer_infos[static_cast<size_t>(top_li)];
    const size_t off = ml.layer_node_offset[static_cast<size_t>(top_li)];
    for (int iy = 0; iy < tl.ny; ++iy) {
      for (int ix = 0; ix < tl.nx; ++ix) {
        const bool on_edge = (ix == 0 || ix == tl.nx - 1
                              || iy == 0 || iy == tl.ny - 1);
        if (on_edge) {
          const size_t idx = off + static_cast<size_t>(iy * tl.nx + ix);
          auto& n = ml.nodes[idx];
          n.is_ring = true;
          n.v_src_V = v_supply_V;
          n.v_V = v_supply_V;
          ml.ring_nodes.push_back(idx);
        }
      }
    }
  }

  return ml;
}

// ---- Multi-layer adjacency-list CG solver ----

void matvec_G_ml(const MultiLayerMesh& ml,
                 const std::vector<double>& x,
                 std::vector<double>& Gx) {
  const size_t N = ml.nodes.size();
  Gx.assign(N, 0.0);
  for (size_t i = 0; i < N; ++i) {
    if (ml.nodes[i].is_ring) {
      Gx[i] = x[i];
      continue;
    }
    double diag = 0.0;
    double off = 0.0;
    for (const auto& edge : ml.adj[i]) {
      diag += edge.second;
      off -= edge.second * x[edge.first];
    }
    Gx[i] = diag * x[i] + off;
  }
}

double diag_G_ml(size_t idx, const MultiLayerMesh& ml) {
  if (ml.nodes[idx].is_ring)
    return 1.0;
  double d = 0.0;
  for (const auto& edge : ml.adj[idx])
    d += edge.second;
  return d;
}

void solve_mesh_cg_ml(MultiLayerMesh& ml,
                      double tol = 1e-10,
                      int max_iter = 10000) {
  const size_t N = ml.nodes.size();
  if (N == 0)
    return;

  std::vector<double> b(N);
  std::vector<double> x(N);
  std::vector<double> diag_inv(N);
  for (size_t i = 0; i < N; ++i) {
    const auto& n = ml.nodes[i];
    if (n.is_ring) {
      b[i] = n.v_src_V;
      x[i] = n.v_src_V;
    } else {
      b[i] = -(n.I_soft_A + n.I_hard_A);
      x[i] = n.v_V;
    }
    const double d = diag_G_ml(i, ml);
    diag_inv[i] = (d > 0.0) ? (1.0 / d) : 1.0;
  }

  std::vector<double> Gx(N);
  matvec_G_ml(ml, x, Gx);
  std::vector<double> r(N);
  for (size_t i = 0; i < N; ++i)
    r[i] = b[i] - Gx[i];

  std::vector<double> z(N);
  double rz = 0.0;
  for (size_t i = 0; i < N; ++i) {
    z[i] = r[i] * diag_inv[i];
    rz += r[i] * z[i];
  }

  std::vector<double> p = z;
  std::vector<double> Ap(N);

  for (int iter = 0; iter < max_iter; ++iter) {
    double r_norm2 = 0.0;
    for (size_t i = 0; i < N; ++i)
      r_norm2 += r[i] * r[i];
    if (r_norm2 < tol * tol)
      break;

    matvec_G_ml(ml, p, Ap);

    double pAp = 0.0;
    for (size_t i = 0; i < N; ++i)
      pAp += p[i] * Ap[i];
    if (pAp <= 0.0)
      break;

    const double alpha = rz / pAp;
    for (size_t i = 0; i < N; ++i) {
      x[i] += alpha * p[i];
      r[i] -= alpha * Ap[i];
    }

    double rz_new = 0.0;
    for (size_t i = 0; i < N; ++i) {
      z[i] = r[i] * diag_inv[i];
      rz_new += r[i] * z[i];
    }

    if (std::abs(rz) < 1e-30)
      break;
    const double beta = rz_new / rz;
    for (size_t i = 0; i < N; ++i)
      p[i] = z[i] + beta * p[i];
    rz = rz_new;
  }

  for (size_t i = 0; i < N; ++i)
    ml.nodes[i].v_V = x[i];
}

// ---- Multi-layer load assignment ----

void assign_mesh_loads_ml(const Chip& chip,
                          const Layout& layout,
                          MultiLayerMesh& ml,
                          const std::vector<HardMacroCurrent>& hard,
                          SoftIrData& soft_data) {
  for (auto& n : ml.nodes) {
    n.I_soft_A = 0.0;
    n.I_hard_A = 0.0;
  }
  if (ml.layers.empty())
    return;

  const double core_lx = layout.core[0];
  const double core_ly = layout.core[1];
  const double core_ux = layout.core[2];
  const double core_uy = layout.core[3];

  // Soft modules attach to the bottom layer (M1).
  const int sli = ml.soft_layer_idx;
  const auto& sl = ml.layers[static_cast<size_t>(sli)];
  const size_t s_off = ml.layer_node_offset[static_cast<size_t>(sli)];

  std::unordered_map<std::string, double> sum_imax_by_name;
  sum_imax_by_name.reserve(soft_data.modules.size());

  for (const auto& mod : soft_data.modules) {
    auto kit = soft_data.knapsack_by_name.find(mod.cluster_name);
    if (kit == soft_data.knapsack_by_name.end())
      continue;
    const FpBox* box = find_fp(chip.fp, mod.cluster_name);
    if (!box)
      continue;
    const SoftKnapsackProfile& kn = kit->second;
    const double bx0 = box->lx;
    const double by0 = box->ly;
    const double bx1 = box->ux;
    const double by1 = box->uy;

    for (int iy = 0; iy < sl.ny; ++iy) {
      for (int ix = 0; ix < sl.nx; ++ix) {
        double tx0, ty0, tx1, ty1;
        node_tile_nu(ix, iy, sl.x_coords, sl.y_coords,
                     core_lx, core_ly, core_ux, core_uy,
                     &tx0, &ty0, &tx1, &ty1);
        double ox0, oy0, ox1, oy1;
        const double A_ov = rect_overlap_um2(tx0, ty0, tx1, ty1,
                                             bx0, by0, bx1, by1,
                                             &ox0, &oy0, &ox1, &oy1);
        if (A_ov <= 0.0)
          continue;
        const double I_add = kn.imax(A_ov);
        if (I_add <= 0.0)
          continue;
        const size_t idx = s_off + static_cast<size_t>(iy * sl.nx + ix);
        ml.nodes[idx].I_soft_A += I_add;
        sum_imax_by_name[mod.cluster_name] += I_add;
      }
    }
  }

  for (auto& m : soft_data.modules) {
    auto it = sum_imax_by_name.find(m.cluster_name);
    m.i_mesh_sum_A = (it != sum_imax_by_name.end()) ? it->second : 0.0;
  }

  // Hard macros attach to the lowest strap layer (M4).
  const int hli = ml.hard_layer_idx;
  const size_t h_off = ml.layer_node_offset[static_cast<size_t>(hli)];
  const size_t h_end = ml.layer_node_offset[static_cast<size_t>(hli + 1)];
  for (const auto& h : hard) {
    const size_t j = nearest_node_manhattan_range(
        h.pin_x_um, h.pin_y_um, ml.nodes, h_off, h_end);
    ml.nodes[j].I_hard_A += h.current_A;
  }
}

// ---- Multi-layer local drop helpers ----

double hard_pin_V_ml(const MultiLayerMesh& ml,
                     double pin_x_um,
                     double pin_y_um,
                     double I_A) {
  if (ml.nodes.empty() || ml.ring_nodes.empty())
    return std::numeric_limits<double>::quiet_NaN();
  const int hli = ml.hard_layer_idx;
  const auto& hl = ml.layers[static_cast<size_t>(hli)];
  const size_t h_off = ml.layer_node_offset[static_cast<size_t>(hli)];
  const size_t h_end = ml.layer_node_offset[static_cast<size_t>(hli + 1)];
  const size_t j = nearest_node_manhattan_range(
      pin_x_um, pin_y_um, ml.nodes, h_off, h_end);
  const auto& ni = ml.nodes[j];
  const double dx = std::abs(pin_x_um - ni.x_um);
  const double dy = std::abs(pin_y_um - ni.y_um);
  return ni.v_V - local_layer_drop(hl, dx, dy, I_A);
}

double soft_tile_worst_V_ml(const MultiLayerMesh& ml,
                            const FpBox& module_box,
                            const SoftKnapsackProfile& kn,
                            const Layout& layout) {
  if (ml.nodes.empty() || ml.ring_nodes.empty())
    return std::numeric_limits<double>::quiet_NaN();

  const int sli = ml.soft_layer_idx;
  const auto& sl = ml.layers[static_cast<size_t>(sli)];
  const size_t s_off = ml.layer_node_offset[static_cast<size_t>(sli)];

  const double core_lx = layout.core[0];
  const double core_ly = layout.core[1];
  const double core_ux = layout.core[2];
  const double core_uy = layout.core[3];

  const double bx0 = module_box.lx;
  const double by0 = module_box.ly;
  const double bx1 = module_box.ux;
  const double by1 = module_box.uy;

  double vmin = std::numeric_limits<double>::infinity();
  for (int iy = 0; iy < sl.ny; ++iy) {
    for (int ix = 0; ix < sl.nx; ++ix) {
      double tx0, ty0, tx1, ty1;
      node_tile_nu(ix, iy, sl.x_coords, sl.y_coords,
                   core_lx, core_ly, core_ux, core_uy,
                   &tx0, &ty0, &tx1, &ty1);
      double ox0, oy0, ox1, oy1;
      const double A_ov = rect_overlap_um2(tx0, ty0, tx1, ty1,
                                           bx0, by0, bx1, by1,
                                           &ox0, &oy0, &ox1, &oy1);
      if (A_ov <= 0.0)
        continue;
      const double I_nk = kn.imax(A_ov);
      if (I_nk <= 0.0)
        continue;
      const size_t idx = s_off + static_cast<size_t>(iy * sl.nx + ix);
      const auto& ni = ml.nodes[idx];
      const double dx = std::max(std::abs(ox1 - ni.x_um),
                                 std::abs(ox0 - ni.x_um));
      const double dy = std::max(std::abs(oy1 - ni.y_um),
                                 std::abs(oy0 - ni.y_um));
      const double v = ni.v_V - local_layer_drop(sl, dx, dy, I_nk);
      if (std::isfinite(v))
        vmin = std::min(vmin, v);
    }
  }
  if (!std::isfinite(vmin))
    return std::numeric_limits<double>::quiet_NaN();
  return vmin;
}

}  // namespace

// =====================================================================
// ---- Public API (IrModel methods) ----
// =====================================================================

double SoftKnapsackProfile::imax(double W) const {
  if (W <= 0.0 || I_.empty())
    return 0.0;
  auto it = std::upper_bound(cumA_.begin(), cumA_.end(), W);
  const size_t i = static_cast<size_t>(it - cumA_.begin());
  const size_t m = I_.size();
  const size_t full = (i == 0) ? 0 : i - 1;
  double out = cumI_[full];
  if (full < m && W > cumA_[full])
    out += (W - cumA_[full]) / A_[full] * I_[full];
  return out;
}

IrModel::IrModel(const Chip& chip) : chip_(&chip) {}

EstOpts& IrModel::estOptions() {
  return est_;
}

const EstOpts& IrModel::estOptions() const {
  return est_;
}

void IrModel::setStrapSheet(const StrapSheetModel& strap) {
  strap_ = strap;
}

const StrapSheetModel& IrModel::strapSheet() const {
  return strap_;
}

void IrModel::buildUniformMesh(double pitch_x_um, double pitch_y_um) {
  multi_layer_ = false;
  mesh_ = make_mesh(chip_->layout, pitch_x_um, pitch_y_um, est_.vdd_V);

  // Generate physical stripe positions from Tech to restrict ring vsrc to
  // stripe-ring crossing points.
  const double core_lx = chip_->layout.core[0];
  const double core_ly = chip_->layout.core[1];
  const double core_ux = chip_->layout.core[2];
  const double core_uy = chip_->layout.core[3];

  // Collect non-followpin core-grid stripes, sort by ascending pitch.
  struct SP {
    double pitch;
    double offset;
  };
  std::vector<SP> picks;
  for (const auto& s : chip_->tech.stripes) {
    if (s.followpins)
      continue;
    if (!s.grid.empty() && s.grid != "grid")
      continue;
    if (s.width <= 0.0 || s.pitch <= 0.0)
      continue;
    picks.push_back({s.pitch, s.offset});
  }
  std::sort(picks.begin(), picks.end(),
            [](const SP& a, const SP& b) { return a.pitch < b.pitch; });

  // Convention: smallest-pitch stripe = vertical (x-positions),
  //             next = horizontal (y-positions).
  std::vector<double> vert_xs, horiz_ys;
  auto gen_positions = [](double offset, double pitch, double lo,
                          double hi) -> std::vector<double> {
    std::vector<double> out;
    if (pitch <= 0.0)
      return out;
    double p = offset;
    while (p > lo)
      p -= pitch;
    while (p < lo)
      p += pitch;
    for (; p <= hi; p += pitch)
      out.push_back(p);
    return out;
  };
  if (picks.size() >= 1)
    vert_xs = gen_positions(picks[0].offset, picks[0].pitch, core_lx, core_ux);
  if (picks.size() >= 2)
    horiz_ys = gen_positions(picks[1].offset, picks[1].pitch, core_ly, core_uy);

  mark_ring_at_stripe_crossings(mesh_, est_.vdd_V, vert_xs, horiz_ys);
}

void IrModel::markOuterRing(double v_supply_V) {
  mark_outer_ring(mesh_, v_supply_V);
}

void IrModel::buildMeshFromPdnDump(
    const PdnDump& dump,
    const std::unordered_map<std::string, double>& rc_r_per_um,
    double v_supply_V) {
  multi_layer_ = false;
  StrapSheetModel strap = strap_;
  mesh_ = build_mesh_from_pdn_dump(chip_->layout, dump, rc_r_per_um,
                                   v_supply_V, &strap);
  if (strap.w_hstrap_um > 0.0 || strap.w_vstrap_um > 0.0)
    strap_ = strap;
}

void IrModel::buildMultiLayerMesh(
    const PdnDump& dump,
    const std::unordered_map<std::string, double>& rc_r_per_um,
    double v_supply_V) {
  multi_layer_ = true;
  ml_mesh_ = build_multilayer_mesh(chip_->layout, chip_->tech, dump,
                                   rc_r_per_um, v_supply_V, est_.via_r_ohm);
}

void IrModel::loadHardMacroCurrents() {
  hard_ = hard_currents(*chip_, est_);
}

void IrModel::analyzeSoftModules() {
  soft_ = analyze_soft_modules(*chip_, est_);
}

void IrModel::assignMeshLoads() {
  if (multi_layer_) {
    assign_mesh_loads_ml(*chip_, chip_->layout, ml_mesh_, hard_, soft_);
  } else {
    assign_mesh_loads_single(*chip_, chip_->layout, mesh_, hard_, soft_);
  }
}

double IrModel::hardPinVoltage(double pin_x_um,
                               double pin_y_um,
                               double current_A) const {
  if (multi_layer_)
    return hard_pin_V_ml(ml_mesh_, pin_x_um, pin_y_um, current_A);
  return hard_pin_V(mesh_, strap_, pin_x_um, pin_y_um, current_A,
                    est_.ir_mode);
}

double IrModel::softClusterWorstVoltage(const FpBox& module_box,
                                        const SoftKnapsackProfile& kn) const {
  if (multi_layer_)
    return soft_tile_worst_V_ml(ml_mesh_, module_box, kn, chip_->layout);
  return soft_tile_worst_V(mesh_, strap_, module_box, kn, chip_->layout,
                           est_.ir_mode);
}

double IrModel::softClusterWorstVoltage(
    const FpBox& module_box,
    const std::string& cluster_name) const {
  auto it = soft_.knapsack_by_name.find(cluster_name);
  if (it == soft_.knapsack_by_name.end())
    return std::numeric_limits<double>::quiet_NaN();
  if (multi_layer_)
    return soft_tile_worst_V_ml(ml_mesh_, module_box, it->second,
                                chip_->layout);
  return soft_tile_worst_V(mesh_, strap_, module_box, it->second,
                           chip_->layout, est_.ir_mode);
}

void IrModel::solveMeshVoltages() {
  if (multi_layer_)
    solve_mesh_cg_ml(ml_mesh_);
  else
    solve_mesh_cg(mesh_, strap_);
}

double IrModel::nodeVoltage(size_t node_idx) const {
  if (multi_layer_) {
    if (node_idx >= ml_mesh_.nodes.size())
      return std::numeric_limits<double>::quiet_NaN();
    const auto& n = ml_mesh_.nodes[node_idx];
    if (n.is_ring)
      return n.v_src_V;
    return n.v_V;
  }
  if (node_idx >= mesh_.nodes.size())
    return std::numeric_limits<double>::quiet_NaN();
  const auto& n = mesh_.nodes[node_idx];
  if (n.is_ring)
    return n.v_src_V;
  if (est_.ir_mode == IrMode::Max)
    return n.v_V;
  const double I = n.I_soft_A + n.I_hard_A;
  return node_voltage_under_load(mesh_, strap_, node_idx, I, est_.ir_mode);
}

void IrModel::updateMeshVoltages() {
  if (multi_layer_) {
    solve_mesh_cg_ml(ml_mesh_);
    return;
  }
  if (est_.ir_mode == IrMode::Max) {
    solveMeshVoltages();
    return;
  }
  for (size_t i = 0; i < mesh_.nodes.size(); ++i) {
    const double v = nodeVoltage(i);
    if (std::isfinite(v))
      mesh_.nodes[i].v_V = v;
  }
}

}  // namespace phys
