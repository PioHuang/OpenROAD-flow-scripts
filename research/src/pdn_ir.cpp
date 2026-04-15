#include <phys/pdn_ir.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace phys {

namespace {

// Split root.fp.txt names that concatenate several macro instances with "||".
std::vector<std::string> fp_tokens(const std::string& name) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < name.size()) {
    const size_t q = name.find("||", p);
    std::string tok = (q == std::string::npos) ? name.substr(p) : name.substr(p, q - p);
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

// Axis-aligned overlap area (µm²); false if disjoint.
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

// Nearest-node tile: midlines between strap nodes; edges clamp to core (paper Fig. 6(b) style).
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

double ir_drop(const StrapSheetModel& strap, double dx_um, double dy_um, double I_A) {
  const double rh = (strap.w_hstrap_um > 0.0) ? (strap.r_sqh * dx_um / strap.w_hstrap_um)
                                              : std::numeric_limits<double>::infinity();
  const double rv = (strap.w_vstrap_um > 0.0) ? (strap.r_sqv * dy_um / strap.w_vstrap_um)
                                              : std::numeric_limits<double>::infinity();
  return I_A * std::max(rh, rv);
}

SoftKnapsackProfile make_profile_from_items(std::vector<std::pair<double, double>> items) {
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

size_t nearest_node_manhattan(double px, double py, const UniformMesh& mesh) {
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

double inst_I(const Instance& inst, const EstOpts& opt) {
  if (inst.has_sta_power) {
    if (opt.prefer_sta_current && inst.sta_power.has_current && inst.sta_power.current_A > 0.0)
      return inst.sta_power.current_A;
    if (opt.vdd_V > 0.0 && inst.sta_power.total_W > 0.0)
      return inst.sta_power.total_W / opt.vdd_V;
  }
  if (inst.has_manual_power && opt.vdd_V > 0.0 && inst.manual_power_W > 0.0)
    return inst.manual_power_W / opt.vdd_V;
  return 0.0;
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
      g.name = inst.cluster_name.empty() ? ("cluster_" + std::to_string(inst.cluster_id))
                                         : inst.cluster_name;
    g.n++;
    if (inst.is_macro)
      g.n_macro++;
    else
      g.n_std++;
    g.I_obs += I;
    if (I > 0.0) {
      const double a = (inst.has_area && inst.area_um2 > 0.0) ? inst.area_um2 : opt.assumed_cell_area_um2;
      g.items.push_back(
          {I, std::max(a, std::numeric_limits<double>::min())});
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

    const double cap_um2 = m.box_area_um2 * std::max(0.0, opt.packing_utilization);
    double I_pack = kn.imax(cap_um2);
    if (cap_um2 <= 0.0)
      I_pack = m.observed_total_A;
    m.worst_case_A = std::max(I_pack, m.observed_total_A);
    m.i_mesh_sum_A = 0.0;
    out.modules.push_back(std::move(m));
  }

  std::sort(out.modules.begin(), out.modules.end(), [](const ModuleCurrent& a, const ModuleCurrent& b) {
    return a.worst_case_A > b.worst_case_A;
  });
  return out;
}

std::vector<HardMacroCurrent> hard_currents(const Chip& chip, const EstOpts& opt) {
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
            + "\" has no ODB PG geometry (instance_geom.tsv). LEF/ODB must expose POWER/GROUND pins.");
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
  std::sort(out.begin(), out.end(), [](const HardMacroCurrent& a, const HardMacroCurrent& b) {
    return a.current_A > b.current_A;
  });
  return out;
}

void assign_mesh_loads(const Chip& chip,
                       const Layout& layout,
                       const EstOpts& opt,
                       UniformMesh& mesh,
                       const std::vector<HardMacroCurrent>& hard,
                       SoftIrData& soft_data) {
  // (void)opt;
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
        node_tile_um(ix, iy, nx, ny, core_lx, core_ly, core_ux, core_uy, px, py, mesh_lx, mesh_ly,
                     &tx0, &ty0, &tx1, &ty1);
        double ix0, iy0, ix1, iy1;
        const double A_ov
            = rect_overlap_um2(tx0, ty0, tx1, ty1, bx0, by0, bx1, by1, &ix0, &iy0, &ix1, &iy1);
        if (A_ov <= 0.0)
          continue;
        // Paper: Imax(A_ov(n,k), k); overlap area only (no extra packing_util on A_ov).
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

double soft_tile_worst_V(const UniformMesh& mesh,
                         const StrapSheetModel& strap,
                         const FpBox& module_box,
                         const SoftKnapsackProfile& kn,
                         const Layout& layout) {
  if (mesh.nodes.empty())
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
      node_tile_um(ix, iy, nx, ny, core_lx, core_ly, core_ux, core_uy, px, py, mesh_lx, mesh_ly,
                   &tx0, &ty0, &tx1, &ty1);
      double ix0, iy0, ix1, iy1;
      const double A_ov
          = rect_overlap_um2(tx0, ty0, tx1, ty1, bx0, by0, bx1, by1, &ix0, &iy0, &ix1, &iy1);
      if (A_ov <= 0.0)
        continue;
      const double I_nk = kn.imax(A_ov);
      if (I_nk <= 0.0)
        continue;
      const size_t idx = static_cast<size_t>(iy * nx + ix);
      const auto& n = mesh.nodes[idx];
      const double dx = std::max(std::abs(n.x_um - ix0), std::abs(n.x_um - ix1));
      const double dy = std::max(std::abs(n.y_um - iy0), std::abs(n.y_um - iy1));
      vmin = std::min(vmin, n.v_V - ir_drop(strap, dx, dy, I_nk));
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
                  double I_A) {
  if (mesh.nodes.empty())
    return std::numeric_limits<double>::quiet_NaN();
  const size_t j = nearest_node_manhattan(pin_x_um, pin_y_um, mesh);
  const auto& near = mesh.nodes[j];
  const double dx = std::abs(pin_x_um - near.x_um);
  const double dy = std::abs(pin_y_um - near.y_um);
  return near.v_V - ir_drop(strap, dx, dy, I_A);
}

void solve_pg_mesh_dc(UniformMesh& mesh,
                      const StrapSheetModel& strap,
                      double v_supply_V,
                      int max_cg_iter,
                      double cg_tol_rel) {
  const int nx = mesh.nx;
  const int ny = mesh.ny;
  const size_t N = mesh.nodes.size();
  if (N == 0)
    return;

  const double px = mesh.pitch_x_um;
  const double py = mesh.pitch_y_um;
  double Gh = 0.0;
  double Gv = 0.0;
  if (strap.w_hstrap_um > 0.0 && strap.r_sqh > 0.0 && px > 0.0)
    Gh = 1.0 / (strap.r_sqh * px / strap.w_hstrap_um);
  if (strap.w_vstrap_um > 0.0 && strap.r_sqv > 0.0 && py > 0.0)
    Gv = 1.0 / (strap.r_sqv * py / strap.w_vstrap_um);

  auto is_bnd = [nx, ny](int ix, int iy) {
    return ix == 0 || ix == nx - 1 || iy == 0 || iy == ny - 1;
  };

  std::vector<int> g2r(N, -1);
  std::vector<int> r2g;
  r2g.reserve(N);
  for (int iy = 0; iy < ny; ++iy) {
    for (int ix = 0; ix < nx; ++ix) {
      const size_t gi = static_cast<size_t>(iy * nx + ix);
      if (!is_bnd(ix, iy)) {
        g2r[gi] = static_cast<int>(r2g.size());
        r2g.push_back(static_cast<int>(gi));
      }
    }
  }
  const int M = static_cast<int>(r2g.size());

  for (size_t gi = 0; gi < N; ++gi) {
    const int ix = static_cast<int>(gi % static_cast<size_t>(nx));
    const int iy = static_cast<int>(gi / static_cast<size_t>(nx));
    if (is_bnd(ix, iy))
      mesh.nodes[gi].v_V = v_supply_V;
  }
  if (M == 0)
    return;

  std::vector<std::vector<double>> A(static_cast<size_t>(M), std::vector<double>(static_cast<size_t>(M), 0.0));
  std::vector<double> b(static_cast<size_t>(M), 0.0);

  for (int ri = 0; ri < M; ++ri) {
    const int gi = r2g[static_cast<size_t>(ri)];
    const int ix = gi % nx;
    const int iy = gi / nx;
    double diag = 0.0;
    double rhs = 0.0;
    auto stamp_nb = [&](int nix, int niy, double G) {
      if (G <= 0.0)
        return;
      diag += G;
      const int gj = niy * nx + nix;
      const int rj = g2r[static_cast<size_t>(gj)];
      if (rj >= 0)
        A[static_cast<size_t>(ri)][static_cast<size_t>(rj)] -= G;
      else
        rhs += G * v_supply_V;
    };
    if (ix + 1 < nx)
      stamp_nb(ix + 1, iy, Gh);
    if (ix > 0)
      stamp_nb(ix - 1, iy, Gh);
    if (iy + 1 < ny)
      stamp_nb(ix, iy + 1, Gv);
    if (iy > 0)
      stamp_nb(ix, iy - 1, Gv);
    A[static_cast<size_t>(ri)][static_cast<size_t>(ri)] = diag;
    const double Iload
        = mesh.nodes[static_cast<size_t>(gi)].I_soft_A + mesh.nodes[static_cast<size_t>(gi)].I_hard_A;
    b[static_cast<size_t>(ri)] = rhs - Iload;
  }

  std::vector<double> x(static_cast<size_t>(M), 0.0);
  std::vector<double> r(static_cast<size_t>(M));
  std::vector<double> p(static_cast<size_t>(M));
  std::vector<double> Ap(static_cast<size_t>(M));

  auto matvec = [&A, M](const std::vector<double>& v, std::vector<double>& out) {
    for (int i = 0; i < M; ++i) {
      double s = 0.0;
      for (int j = 0; j < M; ++j)
        s += A[static_cast<size_t>(i)][static_cast<size_t>(j)] * v[static_cast<size_t>(j)];
      out[static_cast<size_t>(i)] = s;
    }
  };
  auto dot = [M](const std::vector<double>& u, const std::vector<double>& v) {
    double s = 0.0;
    for (int i = 0; i < M; ++i)
      s += u[static_cast<size_t>(i)] * v[static_cast<size_t>(i)];
    return s;
  };

  matvec(x, Ap);
  for (int i = 0; i < M; ++i)
    r[static_cast<size_t>(i)] = b[static_cast<size_t>(i)] - Ap[static_cast<size_t>(i)];
  p = r;

  double rn = dot(r, r);
  const double bnorm = std::sqrt(dot(b, b));
  const double tol = cg_tol_rel * std::max(bnorm, 1e-30);
  const int itmax = (max_cg_iter > 0) ? max_cg_iter : std::min(10000, std::max(200, 10 * M));

  for (int it = 0; it < itmax; ++it) {
    matvec(p, Ap);
    const double denom = dot(p, Ap);
    if (denom <= 1e-300)
      break;
    const double alpha = rn / denom;
    for (int i = 0; i < M; ++i) {
      x[static_cast<size_t>(i)] += alpha * p[static_cast<size_t>(i)];
      r[static_cast<size_t>(i)] -= alpha * Ap[static_cast<size_t>(i)];
    }
    const double rn_new = dot(r, r);
    if (std::sqrt(rn_new) <= tol)
      break;
    const double beta = rn_new / rn;
    rn = rn_new;
    for (int i = 0; i < M; ++i)
      p[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] + beta * p[static_cast<size_t>(i)];
  }

  for (int ri = 0; ri < M; ++ri) {
    const int gi = r2g[static_cast<size_t>(ri)];
    mesh.nodes[static_cast<size_t>(gi)].v_V = x[static_cast<size_t>(ri)];
  }
}

}  // namespace

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
  mesh_ = make_mesh(chip_->layout, pitch_x_um, pitch_y_um, est_.vdd_V);
}

void IrModel::loadHardMacroCurrents() {
  hard_ = hard_currents(*chip_, est_);
}

void IrModel::analyzeSoftModules() {
  soft_ = analyze_soft_modules(*chip_, est_);
}

void IrModel::assignMeshLoads() {
  assign_mesh_loads(*chip_, chip_->layout, est_, mesh_, hard_, soft_);
}

void IrModel::solvePgMeshDc(double v_supply_V, int max_cg_iter, double cg_tol_rel) {
  solve_pg_mesh_dc(mesh_, strap_, v_supply_V, max_cg_iter, cg_tol_rel);
}

double IrModel::hardPinVoltage(double pin_x_um, double pin_y_um, double current_A) const {
  return hard_pin_V(mesh_, strap_, pin_x_um, pin_y_um, current_A);
}

double IrModel::softClusterWorstVoltage(const FpBox& module_box,
                                        const SoftKnapsackProfile& kn) const {
  return soft_tile_worst_V(mesh_, strap_, module_box, kn, chip_->layout);
}

double IrModel::softClusterWorstVoltage(const FpBox& module_box,
                                        const std::string& cluster_name) const {
  auto it = soft_.knapsack_by_name.find(cluster_name);
  if (it == soft_.knapsack_by_name.end())
    return std::numeric_limits<double>::quiet_NaN();
  return soft_tile_worst_V(mesh_, strap_, module_box, it->second, chip_->layout);
}

}  // namespace phys
