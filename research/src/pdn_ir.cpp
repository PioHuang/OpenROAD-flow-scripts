// PDN mesh IR: core grid loads, PSM sources, multi-layer DC (per-strap pitch + pdn_vias at x,y).
#include <phys/pdn_ir.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

// Anonymous helpers: floorplan tokens, overlap, mesh tiles, knapsack prep, PSM I/O, and DC assembly.

namespace phys {

StrapSheetModel::StrapSheetModel(double sheet_r_horizontal,
                                 double sheet_r_vertical,
                                 double horizontal_strap_width_um,
                                 double vertical_strap_width_um)
    : _sheetResistanceHorizontal(sheet_r_horizontal),
      _sheetResistanceVertical(sheet_r_vertical),
      _horizontalStrapWidthUm(horizontal_strap_width_um),
      _verticalStrapWidthUm(vertical_strap_width_um) {}

namespace {

vector<string> fp_tokens(const string& name) {
  vector<string> out;
  size_t p = 0;
  while (p < name.size()) {
    const size_t q = name.find("||", p);
    string tok = (q == string::npos) ? name.substr(p) : name.substr(p, q - p);
    if (!tok.empty())
      out.push_back(move(tok));
    if (q == string::npos)
      break;
    p = q + 2;
  }
  return out;
}

bool fp_is_hard(const Chip& chip, const FpBox& box) {
  const auto toks = fp_tokens(box.regionName());
  if (toks.empty())
    return false;
  for (const auto& t : toks) {
    auto it = chip.instances().find(t);
    if (it == chip.instances().end())
      return false;
    if (!it->second.isMacro())
      return false;
  }
  return true;
}

double fp_area_um2(const FpBox& b) {
  return b.area();
}

const FpBox* find_fp(const vector<FpBox>& fp, const string& name) {
  for (const auto& b : fp) {
    if (b.regionName() == name)
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
  const double x0 = max(ax0, bx0);
  const double y0 = max(ay0, by0);
  const double x1 = min(ax1, bx1);
  const double y1 = min(ay1, by1);
  const double w = max(0.0, x1 - x0);
  const double h = max(0.0, y1 - y0);
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

double ir_drop(const StrapSheetModel& strap, double dx_um, double dy_um, double I_A) {
  const double w_h = strap.horizontalStrapWidthUm();
  const double w_v = strap.verticalStrapWidthUm();
  const double rh = (w_h > 0.0) ? (strap.sheetResistanceHorizontal() * dx_um / w_h)
                                : numeric_limits<double>::infinity();
  const double rv = (w_v > 0.0) ? (strap.sheetResistanceVertical() * dy_um / w_v)
                                : numeric_limits<double>::infinity();
  return I_A * max(rh, rv);
}

/// Picks the mesh node minimizing L1 distance from `(px,py)` (stable tie-break toward lower index).
size_t nearest_node_manhattan(double px, double py, const UniformMesh& mesh) {
  size_t best = 0;
  double best_l1 = numeric_limits<double>::infinity();
  const auto& nodes = mesh.nodes();
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    const double d = abs(px - n.xUm()) + abs(py - n.yUm());
    if (d < best_l1) {
      best_l1 = d;
      best = i;
    }
  }
  return best;
}

vector<double> stripe_centers_1d(double off, double pitch, double half_w, double lo, double hi) {
  vector<double> out;
  if (pitch <= 0.0)
    return out;
  const int k_min = static_cast<int>(floor((lo - off + half_w) / pitch)) - 4;
  const int k_max = static_cast<int>(ceil((hi - off - half_w) / pitch)) + 4;
  for (int k = k_min; k <= k_max; ++k) {
    const double c = off + static_cast<double>(k) * pitch;
    if (c - half_w >= lo - 1e-9 && c + half_w <= hi + 1e-9)
      out.push_back(c);
  }
  return out;
}

/// Fills a regular `nx*ny` grid on `layout.core()` with pitch `(pitch_x_um,pitch_y_um)` and
/// initializes every node to `v_init_V`.
UniformMesh make_mesh(const Layout& layout,
                      double pitch_x_um,
                      double pitch_y_um,
                      double v_init_V) {
  UniformMesh mesh;
  mesh.setDimensions(0, 0, pitch_x_um, pitch_y_um);
  const double lx = layout.core().llx();
  const double ly = layout.core().lly();
  const double ux = layout.core().urx();
  const double uy = layout.core().ury();

  if (pitch_x_um <= 0.0 || pitch_y_um <= 0.0 || ux <= lx || uy <= ly)
    return mesh;

  const int nx = static_cast<int>(floor((ux - lx) / pitch_x_um)) + 1;
  const int ny = static_cast<int>(floor((uy - ly) / pitch_y_um)) + 1;
  mesh.setDimensions(nx, ny, pitch_x_um, pitch_y_um);

  auto& nodes = mesh.nodes();
  nodes.reserve(static_cast<size_t>(nx * ny));
  for (int iy = 0; iy < ny; ++iy) {
    for (int ix = 0; ix < nx; ++ix) {
      MeshNode n;
      n.setGridIndices(ix, iy);
      n.setPositionUm(lx + ix * pitch_x_um, ly + iy * pitch_y_um);
      n.setVoltageV(v_init_V);
      nodes.push_back(move(n));
    }
  }
  return mesh;
}

int nearest_node_on_layer(double px,
                          double py,
                          const vector<int>& layer_nodes,
                          const vector<double>& nx,
                          const vector<double>& ny) {
  if (layer_nodes.empty())
    return -1;
  int best = layer_nodes.front();
  double best_l1 = numeric_limits<double>::infinity();
  for (int gi : layer_nodes) {
    const double d = abs(px - nx[static_cast<size_t>(gi)]) + abs(py - ny[static_cast<size_t>(gi)]);
    if (d < best_l1) {
      best_l1 = d;
      best = gi;
    }
  }
  return best;
}

double instance_supply_current_A(const Instance& inst, const EstimationOptions& opt) {
  if (inst.hasStaPower()) {
    const auto& sp = inst.staPower();
    if (opt.preferStaCurrent() && sp.has_current && sp.current_A > 0.0)
      return sp.current_A;
    if (opt.supplyVoltageV() > 0.0 && sp.total_W > 0.0)
      return sp.total_W / opt.supplyVoltageV();
  }
  if (inst.hasManualPower() && opt.supplyVoltageV() > 0.0 && inst.manualPowerW() > 0.0)
    return inst.manualPowerW() / opt.supplyVoltageV();
  return 0.0;
}

/// Aggregates STA/manual current by RTLMP cluster and builds knapsack profiles for soft IR tiles.
SoftModuleIrData analyze_soft_modules(const Chip& chip, const EstimationOptions& opt) {
  struct Agg {
    string name;
    size_t n{};
    size_t n_std{};
    size_t n_macro{};
    double I_obs{};
    vector<pair<double, double>> items;
  };
  unordered_map<int, Agg> clusters;
  clusters.reserve(chip.instances().size() / 2);

  for (const auto& kv : chip.instances()) {
    const auto& inst = kv.second;
    if (!inst.hasCluster())
      continue;
    const double I = instance_supply_current_A(inst, opt);
    auto& g = clusters[inst.clusterId()];
    if (g.name.empty())
      g.name = inst.clusterName().empty() ? ("cluster_" + to_string(inst.clusterId()))
                                           : inst.clusterName();
    g.n++;
    if (inst.isMacro())
      g.n_macro++;
    else
      g.n_std++;
    g.I_obs += I;
    if (I > 0.0) {
      const double a = (inst.hasArea() && inst.areaUm2() > 0.0) ? inst.areaUm2() : opt.assumedCellAreaUm2();
      g.items.push_back({I, max(a, numeric_limits<double>::min())});
    }
  }

  SoftModuleIrData out;
  for (auto& kv : clusters) {
    const int cid = kv.first;
    auto& g = kv.second;
    if (g.n_std == 0)
      continue;

    SoftModuleCurrent m;
    m.cluster_id = cid;
    m.cluster_name = g.name;
    m.instance_count = g.n;
    m.observed_total_A = g.I_obs;

    if (const FpBox* b = find_fp(chip.floorplanRegions(), g.name))
      m.box_area_um2 = fp_area_um2(*b);

    SoftKnapsackProfile kn;
    kn.rebuildFromItemPairs(move(g.items));
    out.knapsackByClusterName()[m.cluster_name] = move(kn);

    const double cap_um2 = m.box_area_um2 * max(0.0, opt.packingUtilization());
    double I_pack = out.knapsackByClusterName().at(m.cluster_name).maxCurrentForAreaBudget(cap_um2);
    if (cap_um2 <= 0.0)
      I_pack = m.observed_total_A;
    m.worst_case_A = max(I_pack, m.observed_total_A);
    m.i_mesh_sum_A = 0.0;
    out.modules().push_back(move(m));
  }

  sort(out.modules().begin(), out.modules().end(), [](const SoftModuleCurrent& a, const SoftModuleCurrent& b) {
    return a.worst_case_A > b.worst_case_A;
  });
  return out;
}

/// Collects hard-macro PG pin currents from floorplan macro boxes and instance pin geometry.
vector<HardMacroPinCurrent> hard_currents(const Chip& chip, const EstimationOptions& opt) {
  vector<HardMacroPinCurrent> out;
  for (const auto& box : chip.floorplanRegions()) {
    if (!fp_is_hard(chip, box))
      continue;
    const auto toks = fp_tokens(box.regionName());
    if (toks.empty())
      continue;

    for (const auto& t : toks) {
      auto it = chip.instances().find(t);
      if (it == chip.instances().end()) {
        throw runtime_error(
            "hard_currents: instance \"" + t
            + "\" not in Chip::instances (required for hard-macro PG pins).");
      }
      const auto& inst = it->second;
      const double I_tot = instance_supply_current_A(inst, opt);
      if (inst.hasPgPins() && !inst.pgPins().empty()) {
        const double n_pin = static_cast<double>(inst.pgPins().size());
        const double I_each = I_tot / n_pin;
        for (const auto& pin : inst.pgPins()) {
          HardMacroPinCurrent h;
          h.instance = t;
          h.fp_region = box.regionName();
          h.pg_pin_name = pin.name;
          h.current_A = I_each;
          h.pin_x_um = pin.x_um;
          h.pin_y_um = pin.y_um;
          out.push_back(move(h));
        }
      } else {
        HardMacroPinCurrent h;
        h.instance = t;
        h.fp_region = box.regionName();
        h.pg_pin_name = "CENTER_FALLBACK";
        h.current_A = I_tot;
        if (inst.hasLocation()) {
          h.pin_x_um = inst.centerXUm();
          h.pin_y_um = inst.centerYUm();
        } else {
          h.pin_x_um = (box.llx() + box.urx()) * 0.5;
          h.pin_y_um = (box.lly() + box.ury()) * 0.5;
        }
        out.push_back(move(h));
      }
    }
  }
  sort(out.begin(), out.end(), [](const HardMacroPinCurrent& a, const HardMacroPinCurrent& b) {
    return a.current_A > b.current_A;
  });
  return out;
}

/// Deposits hard pin currents and soft-cluster worst-case budgets onto the **load** mesh nodes
/// (nearest-node and tile overlap rules live here).
void assign_mesh_loads(const Chip& chip,
                       const Layout& layout,
                       const EstimationOptions& opt,
                       UniformMesh& mesh,
                       const vector<HardMacroPinCurrent>& hard,
                       SoftModuleIrData& soft_data) {
  (void)opt;
  for (auto& n : mesh.nodes()) {
    n.resetLoads();
  }

  const double core_lx = layout.core().llx();
  const double core_ly = layout.core().lly();
  const double core_ux = layout.core().urx();
  const double core_uy = layout.core().ury();
  const double mesh_lx = core_lx;
  const double mesh_ly = core_ly;
  const int nx = mesh.countX();
  const int ny = mesh.countY();
  const double px = mesh.pitchXUm();
  const double py = mesh.pitchYUm();

  unordered_map<string, double> sum_imax_by_name;
  sum_imax_by_name.reserve(soft_data.modules().size());

  for (const auto& mod : soft_data.modules()) {
    auto kit = soft_data.knapsackByClusterName().find(mod.cluster_name);
    if (kit == soft_data.knapsackByClusterName().end())
      continue;
    const FpBox* box = find_fp(chip.floorplanRegions(), mod.cluster_name);
    if (!box)
      continue;
    const SoftKnapsackProfile& kn = kit->second;
    const double bx0 = box->llx();
    const double by0 = box->lly();
    const double bx1 = box->urx();
    const double by1 = box->ury();

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
        const double I_add = kn.maxCurrentForAreaBudget(A_ov);
        if (I_add <= 0.0)
          continue;
        const size_t idx = static_cast<size_t>(iy * nx + ix);
        mesh.nodes()[idx].addSoftCurrentA(I_add);
        sum_imax_by_name[mod.cluster_name] += I_add;
      }
    }
  }

  for (auto& m : soft_data.modules()) {
    auto it = sum_imax_by_name.find(m.cluster_name);
    m.i_mesh_sum_A = (it != sum_imax_by_name.end()) ? it->second : 0.0;
  }

  for (const auto& h : hard) {
    const size_t j = nearest_node_manhattan(h.pin_x_um, h.pin_y_um, mesh);
    mesh.nodes()[j].addHardCurrentA(h.current_A);
  }
}

double soft_tile_worst_V(const UniformMesh& mesh,
                         const StrapSheetModel& strap,
                         const FpBox& module_box,
                         const SoftKnapsackProfile& kn,
                         const Layout& layout) {
  if (mesh.nodes().empty())
    return numeric_limits<double>::quiet_NaN();

  const double core_lx = layout.core().llx();
  const double core_ly = layout.core().lly();
  const double core_ux = layout.core().urx();
  const double core_uy = layout.core().ury();
  const double mesh_lx = core_lx;
  const double mesh_ly = core_ly;
  const int nx = mesh.countX();
  const int ny = mesh.countY();
  const double px = mesh.pitchXUm();
  const double py = mesh.pitchYUm();

  const double bx0 = module_box.llx();
  const double by0 = module_box.lly();
  const double bx1 = module_box.urx();
  const double by1 = module_box.ury();

  double vmin = numeric_limits<double>::infinity();
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
      const double I_nk = kn.maxCurrentForAreaBudget(A_ov);
      if (I_nk <= 0.0)
        continue;
      const size_t idx = static_cast<size_t>(iy * nx + ix);
      const auto& n = mesh.nodes()[idx];
      const double dx = max(abs(n.xUm() - ix0), abs(n.xUm() - ix1));
      const double dy = max(abs(n.yUm() - iy0), abs(n.yUm() - iy1));
      vmin = min(vmin, n.voltageV() - ir_drop(strap, dx, dy, I_nk));
    }
  }
  if (!isfinite(vmin))
    return numeric_limits<double>::quiet_NaN();
  return vmin;
}

double hard_pin_V(const UniformMesh& mesh,
                  const StrapSheetModel& strap,
                  double pin_x_um,
                  double pin_y_um,
                  double I_A) {
  if (mesh.nodes().empty())
    return numeric_limits<double>::quiet_NaN();
  const size_t j = nearest_node_manhattan(pin_x_um, pin_y_um, mesh);
  const auto& near = mesh.nodes()[j];
  const double dx = abs(pin_x_um - near.xUm());
  const double dy = abs(pin_y_um - near.yUm());
  return near.voltageV() - ir_drop(strap, dx, dy, I_A);
}

}  // namespace

namespace {

bool point_in_rect_um(double x, double y, const Rectangle& r) {
  return x >= r.llx() && x <= r.urx() && y >= r.lly() && y <= r.ury();
}

string trim_vsrc_token(string s) {
  while (!s.empty() && isspace(static_cast<unsigned char>(s.front())))
    s.erase(0, 1);
  while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
    s.pop_back();
  return s;
}

vector<Rectangle> parse_psm_vsrc_rects_um(const fs::path& path) {
  // tools/OpenROAD/src/psm/src/ir_solver.cpp generateSourceNodesFromSourceFile — x_um, y_um, size_um, voltage_V
  ifstream in(path);
  if (!in)
    throw runtime_error("psm_vsrc_file: cannot read: " + path.string());
  vector<Rectangle> rects;
  string line;
  size_t lineno = 0;
  while (getline(in, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#')
      continue;
    stringstream ls(line);
    string c0, c1, c2, c3;
    if (!getline(ls, c0, ',') || !getline(ls, c1, ',') || !getline(ls, c2, ',')
        || !getline(ls, c3, ','))
      throw runtime_error("psm_vsrc_file: line " + to_string(lineno) + ": expected x,y,size,voltage");
    const double x = stod(trim_vsrc_token(c0));
    const double y = stod(trim_vsrc_token(c1));
    const double size_um = stod(trim_vsrc_token(c2));
    (void)stod(trim_vsrc_token(c3));
    const double h = size_um * 0.5;
    rects.emplace_back(Rectangle(x - h, y - h, x + h, y + h));
  }
  if (rects.empty())
    throw runtime_error("psm_vsrc_file: no valid source lines: " + path.string());
  return rects;
}

vector<string> split_psm_box_tokens(const string& line) {
  const char delim = (count(line.begin(), line.end(), '\t') >= 4) ? '\t' : ',';
  vector<string> tok;
  stringstream ss(line);
  string p;
  while (getline(ss, p, delim)) {
    const string t = trim_vsrc_token(p);
    if (!t.empty())
      tok.push_back(t);
  }
  return tok;
}

vector<Rectangle> parse_psm_vsrc_boxes_um(const fs::path& path) {
  ifstream in(path);
  if (!in)
    throw runtime_error("psm_vsrc_boxes_file: cannot read: " + path.string());
  vector<Rectangle> rects;
  string line;
  size_t lineno = 0;
  while (getline(in, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#')
      continue;
    const vector<string> tok = split_psm_box_tokens(line);
    if (tok.size() >= 5 && tok[0] == "llx_um")
      continue;
    if (tok.size() < 5)
      throw runtime_error("psm_vsrc_boxes_file: line " + to_string(lineno)
                          + ": expected llx lly urx ury voltage (tab or comma)");
    const double llx = stod(tok[0]);
    const double lly = stod(tok[1]);
    const double urx = stod(tok[2]);
    const double ury = stod(tok[3]);
    (void)stod(tok[4]);
    rects.emplace_back(Rectangle(llx, lly, urx, ury));
  }
  if (rects.empty())
    throw runtime_error("psm_vsrc_boxes_file: no valid box lines: " + path.string());
  return rects;
}

struct ViaRow {
  double x{}, y{};
  string lo, hi;
};

struct ShapeRow {
  string net;
  string sig;
  string kind;
  string layer;
  double llx{}, lly{}, urx{}, ury{};
};

vector<ViaRow> parse_pdn_vias_tsv(const fs::path& path) {
  ifstream in(path);
  if (!in)
    throw runtime_error("pdn_vias_file: cannot read: " + path.string());
  vector<ViaRow> vias;
  string line;
  size_t lineno = 0;
  while (getline(in, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#')
      continue;
    const vector<string> tok = split_psm_box_tokens(line);
    if (tok.size() >= 10 && tok[0] == "x_um")
      continue;
    if (tok.size() < 8)
      throw runtime_error("pdn_vias_file: line " + to_string(lineno)
                          + ": expected x y llx lly urx ury lower upper ...");
    ViaRow v;
    v.x = stod(tok[0]);
    v.y = stod(tok[1]);
    v.lo = tok[6];
    v.hi = tok[7];
    vias.push_back(move(v));
  }
  return vias;
}

vector<ShapeRow> parse_pdn_shapes_tsv(const fs::path& path) {
  ifstream in(path);
  if (!in)
    throw runtime_error("pdn_shapes_file: cannot read: " + path.string());
  vector<ShapeRow> rows;
  string line;
  size_t lineno = 0;
  while (getline(in, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#')
      continue;
    const vector<string> tok = split_psm_box_tokens(line);
    if (tok.size() >= 10 && tok[0] == "net")
      continue;
    if (tok.size() < 10)
      continue;
    ShapeRow r;
    r.net = tok[0];
    r.sig = tok[1];
    r.kind = tok[2];
    r.layer = tok[3];
    r.llx = stod(tok[4]);
    r.lly = stod(tok[5]);
    r.urx = stod(tok[6]);
    r.ury = stod(tok[7]);
    rows.push_back(move(r));
  }
  return rows;
}

vector<Rectangle> clip_source_rects_to_core(const vector<Rectangle>& raw, const Rectangle& core) {
  vector<Rectangle> out;
  out.reserve(raw.size());
  for (const auto& r : raw) {
    if (const auto c = intersect_rectangles(r, core))
      out.push_back(*c);
  }
  return out;
}

vector<Rectangle> source_rect_centers_only(const vector<Rectangle>& rects) {
  vector<Rectangle> out;
  out.reserve(rects.size());
  for (const auto& r : rects) {
    const double cx = 0.5 * (r.llx() + r.urx());
    const double cy = 0.5 * (r.lly() + r.ury());
    // Model each source by its geometric center (e.g. BTerm center).
    out.emplace_back(Rectangle(cx, cy, cx, cy));
  }
  return out;
}

void mark_mesh_nodes_fixed_from_psm_rects(const UniformMesh& mesh,
                                          const vector<Rectangle>& rects,
                                          vector<uint8_t>& fixed,
                                          bool one_node_per_source_rect) {
  const size_t N = mesh.nodes().size();
  if (rects.empty())
    throw runtime_error("psm: empty voltage source rectangle list");
  for (const auto& r : rects) {
    if (one_node_per_source_rect) {
      vector<size_t> inside;
      inside.reserve(32);
      for (size_t gi = 0; gi < N; ++gi) {
        const auto& n = mesh.nodes()[gi];
        if (point_in_rect_um(n.xUm(), n.yUm(), r))
          inside.push_back(gi);
      }
      if (!inside.empty()) {
        const double cx = (r.llx() + r.urx()) * 0.5;
        const double cy = (r.lly() + r.ury()) * 0.5;
        size_t best = inside[0];
        double best_d = hypot(mesh.nodes()[best].xUm() - cx, mesh.nodes()[best].yUm() - cy);
        for (size_t k = 1; k < inside.size(); ++k) {
          const size_t gi = inside[k];
          const double d = hypot(mesh.nodes()[gi].xUm() - cx, mesh.nodes()[gi].yUm() - cy);
          if (d < best_d || (d == best_d && gi < best)) {
            best_d = d;
            best = gi;
          }
        }
        fixed[best] = 1;
      } else {
        throw runtime_error("psm: source rectangle contains no mesh nodes (one-node-per-rect mode)");
      }
      continue;
    }

    bool any_inside = false;
    for (size_t gi = 0; gi < N; ++gi) {
      const auto& n = mesh.nodes()[gi];
      if (point_in_rect_um(n.xUm(), n.yUm(), r)) {
        fixed[gi] = 1;
        any_inside = true;
      }
    }
    if (!any_inside) {
      // Fallback to nearest-node center anchoring when no node lies inside.
      const double cx = (r.llx() + r.urx()) * 0.5;
      const double cy = (r.lly() + r.ury()) * 0.5;
      size_t best = 0;
      double best_d = numeric_limits<double>::infinity();
      for (size_t gi = 0; gi < N; ++gi) {
        const auto& n = mesh.nodes()[gi];
        const double d = hypot(n.xUm() - cx, n.yUm() - cy);
        if (d < best_d || (d == best_d && gi < best)) {
          best_d = d;
          best = gi;
        }
      }
      fixed[best] = 1;
    }
  }
  size_t nf = 0;
  for (uint8_t f : fixed)
    nf += static_cast<size_t>(f);
  if (nf == 0)
    throw runtime_error("psm: could not fix any mesh node as voltage source");
}

int metal_tier(const string& layer) {
  int best = -1, run = -1;
  for (char c : layer) {
    if (isdigit(static_cast<unsigned char>(c))) {
      if (run < 0)
        run = 0;
      run = run * 10 + (c - '0');
      best = run;
    } else
      run = -1;
  }
  return best;
}

void run_cg(int m,
            const vector<double>& diag,
            const vector<vector<pair<int, double>>>& off,
            const vector<double>& rhs,
            vector<double>& x,
            int max_cg_iter,
            double cg_tol_rel) {
  vector<double> r(m), z(m), p(m), Ap(m), Minv(m);
  for (int i = 0; i < m; ++i) {
    const double d = diag[static_cast<size_t>(i)];
    Minv[static_cast<size_t>(i)] = (d > 1e-30) ? (1.0 / d) : 0.0;
  }
  auto mv = [&](const vector<double>& v, vector<double>& out) {
    for (int i = 0; i < m; ++i) {
      double s = diag[static_cast<size_t>(i)] * v[static_cast<size_t>(i)];
      for (const auto& e : off[static_cast<size_t>(i)])
        s += e.second * v[static_cast<size_t>(e.first)];
      out[static_cast<size_t>(i)] = s;
    }
  };
  auto dot = [&](const vector<double>& u, const vector<double>& v) {
    double s = 0.0;
    for (int i = 0; i < m; ++i)
      s += u[static_cast<size_t>(i)] * v[static_cast<size_t>(i)];
    return s;
  };
  mv(x, Ap);
  for (int i = 0; i < m; ++i) {
    r[static_cast<size_t>(i)] = rhs[static_cast<size_t>(i)] - Ap[static_cast<size_t>(i)];
    z[static_cast<size_t>(i)] = Minv[static_cast<size_t>(i)] * r[static_cast<size_t>(i)];
    p[static_cast<size_t>(i)] = z[static_cast<size_t>(i)];
  }
  double rz = dot(r, z);
  const double bn = sqrt(dot(rhs, rhs));
  const double tol = cg_tol_rel * max(bn, 1e-30);
  const int itmax = (max_cg_iter > 0) ? max_cg_iter : min(20000, max(300, 20 * m));
  for (int it = 0; it < itmax; ++it) {
    mv(p, Ap);
    const double denom = dot(p, Ap);
    if (denom <= 1e-300)
      break;
    const double alpha = rz / denom;
    for (int i = 0; i < m; ++i) {
      x[static_cast<size_t>(i)] += alpha * p[static_cast<size_t>(i)];
      r[static_cast<size_t>(i)] -= alpha * Ap[static_cast<size_t>(i)];
    }
    if (sqrt(dot(r, r)) <= tol)
      break;
    for (int i = 0; i < m; ++i)
      z[static_cast<size_t>(i)] = Minv[static_cast<size_t>(i)] * r[static_cast<size_t>(i)];
    const double rz_new = dot(r, z);
    if (abs(rz) <= 1e-300)
      break;
    const double beta = rz_new / rz;
    rz = rz_new;
    for (int i = 0; i < m; ++i)
      p[static_cast<size_t>(i)] = z[static_cast<size_t>(i)] + beta * p[static_cast<size_t>(i)];
  }
}

}  // namespace

vector<Rectangle> psm_supply_source_rects_um(const Chip& chip) {
  const Rectangle& core = chip.layout().core();
  vector<Rectangle> raw;

  if (!chip.psmVsrcBoxesFile().empty()) {
    const fs::path p = fs::weakly_canonical(chip.repoRoot() / chip.psmVsrcBoxesFile());
    if (!fs::is_regular_file(p))
      throw runtime_error("psm_vsrc_boxes_file not found: " + p.string());
    raw = parse_psm_vsrc_boxes_um(p);
  } else if (!chip.psmVsrcFile().empty()) {
    const fs::path p = fs::weakly_canonical(chip.repoRoot() / chip.psmVsrcFile());
    if (!fs::is_regular_file(p))
      throw runtime_error("psm_vsrc_file not found: " + p.string());
    raw = parse_psm_vsrc_rects_um(p);
  } else {
    throw runtime_error("manifest must set psm_vsrc_boxes_file or psm_vsrc_file for VDD sources");
  }

  vector<Rectangle> clipped = clip_source_rects_to_core(raw, core);
  if (clipped.empty())
    throw runtime_error("psm: all source geometry lies outside layout.core after clip");
  return source_rect_centers_only(clipped);
}

// Multi-layer DC: one mesh per strap pitch; vias from `pdn_vias_file` at (x,y) when set, else synthetic
// vias at each load-mesh node for each PDN layer pair. `mesh` is the load layer.
void solve_pg_mesh_dc(UniformMesh& mesh,
                      const StrapSheetModel& strap,
                      const PdnModel& pdn,
                      const vector<Rectangle>& src_um,
                      double vdd,
                      int max_cg_iter,
                      double cg_tol_rel,
                      bool vsrc_one_node,
                      const Chip& chip,
                      const vector<HardMacroPinCurrent>* hard_pins,
                      vector<HardPinGraphAttach>* hard_pin_attach_out,
                      MultiLayerSolveStats* stats,
                      vector<PdnGraphNode>* graph_nodes_out,
                      vector<PdnGraphEdge>* graph_edges_out) {
  (void)strap;
  const int nx = mesh.countX();
  const int ny = mesh.countY();
  const size_t n_load = mesh.nodes().size();
  if (n_load == 0)
    return;

  unordered_map<string, double> r_um;
  for (const auto& t : chip.tech().layerResistanceCapacitance())
    r_um[t.layer] = t.r;

  if (pdn.layers.empty())
    throw runtime_error("pdn_ir: no add_pdn_stripe with set_layer_rc");

  struct Metal {
    string name;
    bool horiz{};
    double g{};
    int tier{};
    double pitch{};
  };
  vector<Metal> metal;
  unordered_map<string, int> ix;
  for (size_t i = 0; i < pdn.layers.size(); ++i) {
    if (ix.count(pdn.layers[i].name))
      continue;
    ix[pdn.layers[i].name] = static_cast<int>(metal.size());
    metal.push_back({pdn.layers[i].name, pdn.layers[i].isHoriz, pdn.layers[i].gSeg, pdn.layers[i].rank, pdn.layers[i].pitchUm});
  }

  set<pair<int, int>> pairMetalSet;
  for (const auto& e : pdn.viaPairs) {
    if (e.first < 0 || e.second < 0 || static_cast<size_t>(e.first) >= pdn.layers.size() ||
        static_cast<size_t>(e.second) >= pdn.layers.size())
      continue;
    const string& na = pdn.layers[static_cast<size_t>(e.first)].name;
    const string& nb = pdn.layers[static_cast<size_t>(e.second)].name;
    auto pa = ix.find(na), pb = ix.find(nb);
    if (pa == ix.end() || pb == ix.end() || pa->second == pb->second)
      continue;
    int ma = pa->second, mb = pb->second;
    if (ma > mb)
      swap(ma, mb);
    pairMetalSet.emplace(ma, mb);
  }
  vector<pair<int, int>> pairMetal(pairMetalSet.begin(), pairMetalSet.end());

  const int L = static_cast<int>(metal.size());
  if (L >= 2 && pairMetal.empty())
    throw runtime_error(
        "pdn_ir: no connected layer pairs (add_pdn_connect / pdn_vias_file vs fitted add_pdn_stripe layers)");

  int src = 0;
  int load = 0;
  for (int k = 1; k < L; ++k) {
    if (metal[static_cast<size_t>(k)].tier > metal[static_cast<size_t>(src)].tier)
      src = k;
    const int rk = metal[static_cast<size_t>(k)].tier;
    const int r0 = metal[static_cast<size_t>(load)].tier;
    if (rk >= 0 && (r0 < 0 || rk < r0))
      load = k;
  }
  int min_tier = numeric_limits<int>::max();
  for (int k = 0; k < L; ++k) {
    if (metal[static_cast<size_t>(k)].tier >= 0)
      min_tier = min(min_tier, metal[static_cast<size_t>(k)].tier);
  }
  if (min_tier == numeric_limits<int>::max())
    min_tier = metal[static_cast<size_t>(load)].tier;
  vector<uint8_t> is_followpin_like(static_cast<size_t>(L), 0);
  for (int k = 0; k < L; ++k) {
    const auto& m = metal[static_cast<size_t>(k)];
    if (m.tier == min_tier && m.pitch > 0.0 && m.pitch <= 3.0)
      is_followpin_like[static_cast<size_t>(k)] = 1;
  }
  // Default abstraction: if the selected load layer is followpin-like, promote
  // loads to the next-lowest non-followpin layer to avoid huge explicit M1 rail graphs.
  if (is_followpin_like[static_cast<size_t>(load)]) {
    int promoted = -1;
    for (int k = 0; k < L; ++k) {
      if (is_followpin_like[static_cast<size_t>(k)])
        continue;
      if (promoted < 0 || metal[static_cast<size_t>(k)].tier < metal[static_cast<size_t>(promoted)].tier)
        promoted = k;
    }
    if (promoted >= 0)
      load = promoted;
  }

  const bool graph_mode = []() {
    const char* v = getenv("PHYS_PDN_SOLVE_MODE");
    if (v == nullptr)
      return false;
    string s = v;
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s == "graph";
  }();

  if (graph_nodes_out)
    graph_nodes_out->clear();
  if (graph_edges_out)
    graph_edges_out->clear();
  if (hard_pin_attach_out)
    hard_pin_attach_out->clear();

  if (graph_mode && !chip.pdnShapesFile().empty() && !chip.pdnViasFile().empty()) {
    // ODB graph mode (PDNSim-like): nodes from via sites, wire edges from
    // extracted shape rectangles, via edges from exported pdn_vias rows.
    struct GNode {
      int layer{};
      double x{};
      double y{};
    };
    vector<GNode> nodes;
    unordered_map<string, vector<int>> by_layer_ids;
    vector<double> gx;
    vector<double> gy;
    vector<int> gl;
    vector<double> loadI;  // only for load layer nodes
    // Per-layer dedup cache. Followpin-like layers are coarsened aggressively by
    // binning node coordinates to reduce graph size; other layers dedup at 1nm-ish.
    unordered_map<int, unordered_map<long long, int>> layer_key_to_node;
    auto node_key = [&](int li, double x, double y) -> long long {
      double q = 0.001;  // um
      if (is_followpin_like[static_cast<size_t>(li)])
        q = max(10.0, min(mesh.pitchXUm(), mesh.pitchYUm()));
      const long long qx = static_cast<long long>(llround(x / q));
      const long long qy = static_cast<long long>(llround(y / q));
      return (qx << 32) ^ static_cast<unsigned long long>(qy & 0xffffffffULL);
    };

    auto add_node = [&](int li, double x, double y) -> int {
      const long long k = node_key(li, x, y);
      auto& by_key = layer_key_to_node[li];
      auto it = by_key.find(k);
      if (it != by_key.end())
        return it->second;
      GNode n{li, x, y};
      nodes.push_back(n);
      const int id = static_cast<int>(nodes.size() - 1);
      gx.push_back(x);
      gy.push_back(y);
      gl.push_back(li);
      loadI.push_back(0.0);
      by_layer_ids[metal[static_cast<size_t>(li)].name].push_back(id);
      by_key.emplace(k, id);
      return id;
    };

    // Add via-site nodes on incident layers.
    const fs::path via_path = fs::weakly_canonical(chip.repoRoot() / chip.pdnViasFile());
    vector<ViaRow> via_rows = parse_pdn_vias_tsv(via_path);
    for (const auto& row : via_rows) {
      auto pa = ix.find(row.lo), pb = ix.find(row.hi);
      if (pa == ix.end() || pb == ix.end())
        continue;
      add_node(pa->second, row.x, row.y);
      add_node(pb->second, row.x, row.y);
    }

    const int N = static_cast<int>(nodes.size());
    if (N == 0)
      throw runtime_error("pdn_ir(graph): no graph nodes built from ODB vias/mesh nodes");

    vector<unordered_map<int, double>> adj(static_cast<size_t>(N));
    auto link = [&adj, &graph_edges_out](int u, int v, double g, const char* kind) {
      if (u < 0 || v < 0 || u == v || g <= 0.0)
        return;
      adj[static_cast<size_t>(u)][v] += g;
      adj[static_cast<size_t>(v)][u] += g;
      if (graph_edges_out != nullptr) {
        PdnGraphEdge e;
        e.u = u;
        e.v = v;
        e.kind = kind;
        e.g = g;
        graph_edges_out->push_back(move(e));
      }
    };

    // Add in-layer edges from wire shape rectangles.
    const fs::path shape_path = fs::weakly_canonical(chip.repoRoot() / chip.pdnShapesFile());
    const auto shape_rows = parse_pdn_shapes_tsv(shape_path);
    string power_net = "VDD";
    if (!chip.tech().voltageDomains().empty() && !chip.tech().voltageDomains().front().power_net.empty())
      power_net = chip.tech().voltageDomains().front().power_net;

    unordered_map<string, double> r_um;
    for (const auto& t : chip.tech().layerResistanceCapacitance())
      r_um[t.layer] = t.r;

    for (const auto& sr : shape_rows) {
      if (sr.kind != "wire" || sr.sig != "POWER" || sr.net != power_net)
        continue;
      auto il = ix.find(sr.layer);
      if (il == ix.end())
        continue;
      const int li = il->second;
      auto it_nodes = by_layer_ids.find(sr.layer);
      if (it_nodes == by_layer_ids.end() || it_nodes->second.empty())
        continue;
      vector<int> in_rect;
      in_rect.reserve(it_nodes->second.size());
      for (int nid : it_nodes->second) {
        const double x = gx[static_cast<size_t>(nid)];
        const double y = gy[static_cast<size_t>(nid)];
        if (x >= sr.llx - 1e-9 && x <= sr.urx + 1e-9 && y >= sr.lly - 1e-9 && y <= sr.ury + 1e-9)
          in_rect.push_back(nid);
      }
      if (in_rect.size() < 2)
        continue;
      const bool horiz = metal[static_cast<size_t>(li)].horiz;
      sort(in_rect.begin(), in_rect.end(), [&](int a, int b) {
        if (horiz) {
          if (abs(gy[static_cast<size_t>(a)] - gy[static_cast<size_t>(b)]) > 1e-6)
            return gy[static_cast<size_t>(a)] < gy[static_cast<size_t>(b)];
          return gx[static_cast<size_t>(a)] < gx[static_cast<size_t>(b)];
        }
        if (abs(gx[static_cast<size_t>(a)] - gx[static_cast<size_t>(b)]) > 1e-6)
          return gx[static_cast<size_t>(a)] < gx[static_cast<size_t>(b)];
        return gy[static_cast<size_t>(a)] < gy[static_cast<size_t>(b)];
      });
      const double width = horiz ? max(1e-9, sr.ury - sr.lly) : max(1e-9, sr.urx - sr.llx);
      const double rpu = r_um.count(sr.layer) ? r_um[sr.layer] : 0.0;
      for (size_t i = 1; i < in_rect.size(); ++i) {
        const int a = in_rect[i - 1], b = in_rect[i];
        const double d = hypot(gx[static_cast<size_t>(a)] - gx[static_cast<size_t>(b)],
                               gy[static_cast<size_t>(a)] - gy[static_cast<size_t>(b)]);
        if (d <= 1e-9 || rpu <= 0.0)
          continue;
        const double R = rpu * d / width;
        if (R > 0.0)
          link(a, b, 1.0 / R, "wire");
      }
    }

    // Add via edges from explicit via rows.
    double g_via = 0.0;
    for (const auto& e : pairMetal)
      g_via = max(g_via, 0.5 * min(metal[static_cast<size_t>(e.first)].g, metal[static_cast<size_t>(e.second)].g));
    if (g_via <= 0.0)
      g_via = 1.0;
    for (const auto& row : via_rows) {
      auto pa = ix.find(row.lo), pb = ix.find(row.hi);
      if (pa == ix.end() || pb == ix.end())
        continue;
      const string& la = metal[static_cast<size_t>(pa->second)].name;
      const string& lb = metal[static_cast<size_t>(pb->second)].name;
      int na = nearest_node_on_layer(row.x, row.y, by_layer_ids[la], gx, gy);
      int nb = nearest_node_on_layer(row.x, row.y, by_layer_ids[lb], gx, gy);
      link(na, nb, g_via, "via");
    }

    // Voltage sources on top/source layer nearest to source rectangles.
    vector<uint8_t> fix(static_cast<size_t>(N), 0);
    const string src_layer_name = metal[static_cast<size_t>(src)].name;
    const auto& src_ids = by_layer_ids[src_layer_name];
    for (const auto& r : src_um) {
      int best = -1;
      double best_d = numeric_limits<double>::infinity();
      bool have_inside = false;
      const double cx = 0.5 * (r.llx() + r.urx());
      const double cy = 0.5 * (r.lly() + r.ury());
      for (int nid : src_ids) {
        const bool inside = (gx[static_cast<size_t>(nid)] >= r.llx() && gx[static_cast<size_t>(nid)] <= r.urx() &&
                             gy[static_cast<size_t>(nid)] >= r.lly() && gy[static_cast<size_t>(nid)] <= r.ury());
        const double d = abs(gx[static_cast<size_t>(nid)] - cx) + abs(gy[static_cast<size_t>(nid)] - cy);
        if (inside && (!have_inside || d < best_d)) {
          have_inside = true;
          best = nid;
          best_d = d;
        } else if (!have_inside && (best < 0 || d < best_d)) {
          best = nid;
          best_d = d;
        }
      }
      if (best >= 0)
        fix[static_cast<size_t>(best)] = 1;
    }

    // Reachability from fixed/source nodes in the built conductance graph.
    vector<uint8_t> reachable(static_cast<size_t>(N), 0);
    {
      vector<int> q;
      q.reserve(static_cast<size_t>(N));
      size_t qi = 0;
      for (int i = 0; i < N; ++i) {
        if (fix[static_cast<size_t>(i)] != 0) {
          reachable[static_cast<size_t>(i)] = 1;
          q.push_back(i);
        }
      }
      while (qi < q.size()) {
        const int u = q[qi++];
        for (const auto& pr : adj[static_cast<size_t>(u)]) {
          const int v = pr.first;
          const double g = pr.second;
          if (g <= 0.0 || v < 0 || v >= N)
            continue;
          if (reachable[static_cast<size_t>(v)] == 0) {
            reachable[static_cast<size_t>(v)] = 1;
            q.push_back(v);
          }
        }
      }
    }
    auto nearest_reachable = [&](double x, double y, const vector<int>& candidates) -> int {
      int best = -1;
      double best_d = numeric_limits<double>::infinity();
      for (int nid : candidates) {
        if (nid < 0 || nid >= N)
          continue;
        if (reachable[static_cast<size_t>(nid)] == 0)
          continue;
        const double d = abs(gx[static_cast<size_t>(nid)] - x) + abs(gy[static_cast<size_t>(nid)] - y);
        if (d < best_d) {
          best_d = d;
          best = nid;
        }
      }
      if (best >= 0)
        return best;
      // Fallback if no reachable candidate exists: original nearest-node behavior.
      return nearest_node_on_layer(x, y, candidates, gx, gy);
    };

    // Attach current sources:
    //   - soft current to load-layer graph nodes
    //   - hard-macro current from explicit hard-pin list onto nearest graph node (all layers)
    const string load_layer_name = metal[static_cast<size_t>(load)].name;
    const auto& load_ids = by_layer_ids[load_layer_name];
    vector<int> all_layer_ids;
    for (int li = 0; li < L; ++li) {
      const auto it = by_layer_ids.find(metal[static_cast<size_t>(li)].name);
      if (it != by_layer_ids.end() && !it->second.empty())
        all_layer_ids.insert(all_layer_ids.end(), it->second.begin(), it->second.end());
    }
    // Fallback: if unexpected empty graph, use load layer.
    const vector<int>& hard_ids = all_layer_ids.empty() ? load_ids : all_layer_ids;
    for (const auto& mn : mesh.nodes()) {
      const double Is = mn.softCurrentA();
      if (Is != 0.0) {
        const int nid = nearest_reachable(mn.xUm(), mn.yUm(), load_ids);
        if (nid >= 0)
          loadI[static_cast<size_t>(nid)] += Is;
      }
    }
    if (hard_pins != nullptr) {
      for (const auto& hp : *hard_pins) {
        if (hp.current_A == 0.0)
          continue;
        const int nid = nearest_reachable(hp.pin_x_um, hp.pin_y_um, hard_ids);
        if (nid >= 0) {
          loadI[static_cast<size_t>(nid)] += hp.current_A;
          if (hard_pin_attach_out != nullptr) {
            HardPinGraphAttach rec;
            rec.instance = hp.instance;
            rec.fp_region = hp.fp_region;
            rec.pg_pin_name = hp.pg_pin_name;
            rec.pin_x_um = hp.pin_x_um;
            rec.pin_y_um = hp.pin_y_um;
            rec.current_A = hp.current_A;
            rec.node_id = nid;
            rec.node_layer = metal[static_cast<size_t>(gl[static_cast<size_t>(nid)])].name;
            rec.node_x_um = gx[static_cast<size_t>(nid)];
            rec.node_y_um = gy[static_cast<size_t>(nid)];
            rec.manhattan_um = abs(rec.node_x_um - rec.pin_x_um) + abs(rec.node_y_um - rec.pin_y_um);
            hard_pin_attach_out->push_back(move(rec));
          }
        } else if (hard_pin_attach_out != nullptr) {
          HardPinGraphAttach rec;
          rec.instance = hp.instance;
          rec.fp_region = hp.fp_region;
          rec.pg_pin_name = hp.pg_pin_name;
          rec.pin_x_um = hp.pin_x_um;
          rec.pin_y_um = hp.pin_y_um;
          rec.current_A = hp.current_A;
          rec.node_id = -1;
          rec.node_layer = "UNMAPPED";
          rec.manhattan_um = numeric_limits<double>::infinity();
          hard_pin_attach_out->push_back(move(rec));
        }
      }
    }

    vector<int> unk_map(static_cast<size_t>(N), -1);
    vector<int> unk_list;
    unk_list.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
      if (!fix[static_cast<size_t>(i)]) {
        unk_map[static_cast<size_t>(i)] = static_cast<int>(unk_list.size());
        unk_list.push_back(i);
      }
    }
    const int m = static_cast<int>(unk_list.size());
    if (m == 0)
      return;

    vector<double> diag(static_cast<size_t>(m), 0.0);
    vector<double> rhs(static_cast<size_t>(m), 0.0);
    vector<vector<pair<int, double>>> off(static_cast<size_t>(m));
    for (int ri = 0; ri < m; ++ri) {
      const int gi = unk_list[static_cast<size_t>(ri)];
      double dsum = 0.0;
      double rsum = -loadI[static_cast<size_t>(gi)];
      for (const auto& pr : adj[static_cast<size_t>(gi)]) {
        const int nb = pr.first;
        const double g = pr.second;
        if (g <= 0.0)
          continue;
        dsum += g;
        const int rj = unk_map[static_cast<size_t>(nb)];
        if (rj >= 0)
          off[static_cast<size_t>(ri)].push_back({rj, -g});
        else
          rsum += g * vdd;
      }
      diag[static_cast<size_t>(ri)] = dsum;
      rhs[static_cast<size_t>(ri)] = rsum;
    }

    vector<double> xsol(static_cast<size_t>(m), vdd);
    run_cg(m, diag, off, rhs, xsol, max_cg_iter, cg_tol_rel);
    vector<double> vall(static_cast<size_t>(N), vdd);
    for (int ri = 0; ri < m; ++ri)
      vall[static_cast<size_t>(unk_list[static_cast<size_t>(ri)])] = xsol[static_cast<size_t>(ri)];

    // Project solved graph voltages back to exported load mesh nodes.
    for (auto& mn : mesh.nodes()) {
      const int nid = nearest_node_on_layer(mn.xUm(), mn.yUm(), load_ids, gx, gy);
      if (nid >= 0)
        mn.setVoltageV(vall[static_cast<size_t>(nid)]);
      else
        mn.setVoltageV(vdd);
    }

    if (graph_nodes_out != nullptr) {
      graph_nodes_out->reserve(static_cast<size_t>(N));
      for (int i = 0; i < N; ++i) {
        PdnGraphNode n;
        n.id = i;
        n.layer_idx = gl[static_cast<size_t>(i)];
        n.layer_name = metal[static_cast<size_t>(n.layer_idx)].name;
        n.x_um = gx[static_cast<size_t>(i)];
        n.y_um = gy[static_cast<size_t>(i)];
        n.v_v = vall[static_cast<size_t>(i)];
        n.fixed = fix[static_cast<size_t>(i)] != 0;
        n.i_load_a = loadI[static_cast<size_t>(i)];
        graph_nodes_out->push_back(move(n));
      }
    }

    if (stats != nullptr) {
      stats->layers.clear();
      stats->vias.clear();
      stats->source_layer = src;
      stats->load_layer = load;
      stats->total_nodes = N;
      int fixed_cnt = 0;
      for (uint8_t f : fix)
        fixed_cnt += static_cast<int>(f);
      stats->fixed_nodes = fixed_cnt;
      stats->unknown_nodes = m;
      stats->isolated_nodes = 0;
      stats->isolated_loaded_nodes = 0;
      stats->isolated_fixed_nodes = 0;
      for (int li = 0; li < L; ++li) {
        MultiLayerSolveStats::LayerInfo q;
        q.name = metal[static_cast<size_t>(li)].name;
        q.horizontal = metal[static_cast<size_t>(li)].horiz;
        q.g_per_segment = metal[static_cast<size_t>(li)].g;
        q.rank = metal[static_cast<size_t>(li)].tier;
        q.pitch_um = metal[static_cast<size_t>(li)].pitch;
        int cnt = 0;
        double sv = 0.0;
        int unk = 0;
        for (int i = 0; i < N; ++i) {
          if (gl[static_cast<size_t>(i)] != li)
            continue;
          ++cnt;
          sv += vall[static_cast<size_t>(i)];
          if (!fix[static_cast<size_t>(i)])
            ++unk;
        }
        q.node_count = cnt;
        q.avg_v = (cnt > 0) ? (sv / static_cast<double>(cnt)) : vdd;
        q.unknown_nodes = unk;
        stats->layers.push_back(move(q));
      }
      for (const auto& e : pairMetal) {
        MultiLayerSolveStats::ViaInfo v;
        v.from_layer = e.first;
        v.to_layer = e.second;
        v.g_via = g_via;
        stats->vias.push_back(v);
      }
    }
    return;
  }

  struct ViaXY {
    int a{}, b{};
    double x{}, y{};
  };
  vector<ViaXY> vias;
  if (L >= 2) {
    if (!chip.pdnViasFile().empty()) {
      const fs::path p = fs::weakly_canonical(chip.repoRoot() / chip.pdnViasFile());
      for (const auto& row : parse_pdn_vias_tsv(p)) {
        auto pa = ix.find(row.lo);
        auto pb = ix.find(row.hi);
        if (pa == ix.end() || pb == ix.end() || pa->second == pb->second)
          continue;
        int a = pa->second;
        int b = pb->second;
        if (a > b)
          swap(a, b);
        vias.push_back({a, b, row.x, row.y});
      }
    } else {
      // Tcl-only fallback: synthesize via sites from add_pdn_stripe pitch/offset intersections,
      // which is closer to PDNSim's geometry-first spirit than attaching every pair at all load nodes.
      const auto& core = chip.layout().core();
      unordered_map<string, vector<Stripe>> stripes_by_layer;
      for (const auto& s : chip.tech().stripes()) {
        if (s.followpins || s.width <= 0.0 || s.pitch <= 0.0)
          continue;
        stripes_by_layer[s.layer].push_back(s);
      }
      auto pick_primary_stripe = [&stripes_by_layer](const string& layer) -> const Stripe* {
        auto it = stripes_by_layer.find(layer);
        if (it == stripes_by_layer.end() || it->second.empty())
          return nullptr;
        const Stripe* best = &it->second.front();
        for (const auto& s : it->second) {
          if (s.width > best->width)
            best = &s;
        }
        return best;
      };

      for (const auto& pr : pairMetal) {
        const string& la = metal[static_cast<size_t>(pr.first)].name;
        const string& lb = metal[static_cast<size_t>(pr.second)].name;
        const Stripe* sa = pick_primary_stripe(la);
        const Stripe* sb = pick_primary_stripe(lb);
        if (sa == nullptr || sb == nullptr)
          continue;

        const bool ha = metal[static_cast<size_t>(pr.first)].horiz;
        const bool hb = metal[static_cast<size_t>(pr.second)].horiz;
        const double hwa = 0.5 * sa->width;
        const double hwb = 0.5 * sb->width;
        if (ha && !hb) {
          const auto ys = stripe_centers_1d(sa->offset, sa->pitch, hwa, core.lly(), core.ury());
          const auto xs = stripe_centers_1d(sb->offset, sb->pitch, hwb, core.llx(), core.urx());
          for (double yv : ys)
            for (double xv : xs)
              vias.push_back({pr.first, pr.second, xv, yv});
        } else if (!ha && hb) {
          const auto ys = stripe_centers_1d(sb->offset, sb->pitch, hwb, core.lly(), core.ury());
          const auto xs = stripe_centers_1d(sa->offset, sa->pitch, hwa, core.llx(), core.urx());
          for (double yv : ys)
            for (double xv : xs)
              vias.push_back({pr.first, pr.second, xv, yv});
        } else {
          // Parallel layers: place sampled vias at load-mesh nodes as a conservative fallback.
          const size_t step = max<size_t>(1, mesh.nodes().size() / 5000);
          for (size_t i = 0; i < mesh.nodes().size(); i += step)
            vias.push_back({pr.first, pr.second, mesh.nodes()[i].xUm(), mesh.nodes()[i].yUm()});
        }
      }

      if (vias.empty()) {
        for (const auto& pr : pairMetal) {
          for (const auto& n : mesh.nodes())
            vias.push_back({pr.first, pr.second, n.xUm(), n.yUm()});
        }
      }
    }
    if (vias.empty())
      throw runtime_error(
          "pdn_ir: no via (x,y) data: set pdn_vias_file or ensure mesh nodes exist for Tcl-only synthetic vias");
  }

  vector<UniformMesh> grid(static_cast<size_t>(L));
  for (int li = 0; li < L; ++li) {
    if (li == load) {
      grid[static_cast<size_t>(li)].setDimensions(nx, ny, mesh.pitchXUm(), mesh.pitchYUm());
      grid[static_cast<size_t>(li)].nodes() = mesh.nodes();
    } else {
      const double pitch = max(metal[static_cast<size_t>(li)].pitch, 1e-9);
      grid[static_cast<size_t>(li)] = make_mesh(chip.layout(), pitch, pitch, vdd);
      for (auto& q : grid[static_cast<size_t>(li)].nodes()) {
        q.setVoltageV(vdd);
        q.resetLoads();
      }
    }
  }

  vector<size_t> pfx(static_cast<size_t>(L + 1), 0);
  for (int li = 0; li < L; ++li)
    pfx[static_cast<size_t>(li + 1)] =
        pfx[static_cast<size_t>(li)] + grid[static_cast<size_t>(li)].nodes().size();
  const size_t nt = pfx[static_cast<size_t>(L)];
  auto nid = [&pfx](int li, size_t j) -> size_t { return pfx[static_cast<size_t>(li)] + j; };

  vector<unordered_map<size_t, double>> adj(nt);
  auto link = [&adj](size_t u, size_t v, double g) {
    if (g <= 0.0 || u == v)
      return;
    adj[u][v] += g;
    adj[v][u] += g;
  };

  for (int li = 0; li < L; ++li) {
    const UniformMesh& g = grid[static_cast<size_t>(li)];
    const int mx = g.countX();
    const int my = g.countY();
    const double gl = metal[static_cast<size_t>(li)].g;
    const size_t b0 = pfx[static_cast<size_t>(li)];
    const size_t nloc = g.nodes().size();
    for (size_t j = 0; j < nloc; ++j) {
      const int ix = static_cast<int>(j % static_cast<size_t>(mx));
      const int iy = static_cast<int>(j / static_cast<size_t>(mx));
      if (metal[static_cast<size_t>(li)].horiz) {
        if (ix > 0)
          link(b0 + j, b0 + j - 1, gl);
        if (ix + 1 < mx)
          link(b0 + j, b0 + j + 1, gl);
      } else {
        if (iy > 0)
          link(b0 + j, b0 + j - static_cast<size_t>(mx), gl);
        if (iy + 1 < my)
          link(b0 + j, b0 + j + static_cast<size_t>(mx), gl);
      }
    }
  }

  double g_via = 0.0;
  for (const auto& e : pairMetal)
    g_via = max(g_via, 0.5 * min(metal[static_cast<size_t>(e.first)].g,
                                 metal[static_cast<size_t>(e.second)].g));
  if (g_via > 0.0 && L >= 2) {
    for (const auto& v : vias) {
      const size_t ua = nid(v.a, nearest_node_manhattan(v.x, v.y, grid[static_cast<size_t>(v.a)]));
      const size_t ub = nid(v.b, nearest_node_manhattan(v.x, v.y, grid[static_cast<size_t>(v.b)]));
      link(ua, ub, g_via);
    }
  }

  vector<uint8_t> fix(nt, 0);
  {
    vector<uint8_t> fx(grid[static_cast<size_t>(src)].nodes().size(), 0);
    mark_mesh_nodes_fixed_from_psm_rects(grid[static_cast<size_t>(src)], src_um, fx, vsrc_one_node);
    for (size_t j = 0; j < fx.size(); ++j)
      if (fx[j])
        fix[nid(src, j)] = 1;
  }

  vector<int> unk_map(nt, -1);
  vector<size_t> unk_list;
  unk_list.reserve(nt);
  for (size_t i = 0; i < nt; ++i)
    if (!fix[i]) {
      unk_map[i] = static_cast<int>(unk_list.size());
      unk_list.push_back(i);
    }
  const int m = static_cast<int>(unk_list.size());
  int n_fix = 0;
  for (uint8_t f : fix)
    n_fix += static_cast<int>(f);
  if (m == 0)
    return;

  vector<double> diag(static_cast<size_t>(m), 0.0);
  vector<double> rhs(static_cast<size_t>(m), 0.0);
  vector<vector<pair<int, double>>> off(static_cast<size_t>(m));
  for (int ri = 0; ri < m; ++ri) {
    const size_t gi = unk_list[static_cast<size_t>(ri)];
    double d = 0.0;
    double r = 0.0;
    auto touch = [&](size_t nb, double g) {
      if (g <= 0.0)
        return;
      d += g;
      const int rj = unk_map[nb];
      if (rj >= 0)
        off[static_cast<size_t>(ri)].push_back({rj, -g});
      else
        r += g * vdd;
    };
    for (const auto& pr : adj[gi])
      touch(pr.first, pr.second);
    diag[static_cast<size_t>(ri)] = d;
    if (gi >= pfx[static_cast<size_t>(load)] && gi < pfx[static_cast<size_t>(load + 1)]) {
      const size_t j = gi - pfx[static_cast<size_t>(load)];
      r -= grid[static_cast<size_t>(load)].nodes()[j].softCurrentA()
           + grid[static_cast<size_t>(load)].nodes()[j].hardCurrentA();
    }
    rhs[static_cast<size_t>(ri)] = r;
  }

  vector<double> x(static_cast<size_t>(m), vdd);
  run_cg(m, diag, off, rhs, x, max_cg_iter, cg_tol_rel);

  vector<double> vall(nt, vdd);
  for (size_t i = 0; i < nt; ++i)
    if (fix[i])
      vall[i] = vdd;
  for (int ri = 0; ri < m; ++ri)
    vall[unk_list[static_cast<size_t>(ri)]] = x[static_cast<size_t>(ri)];

  const size_t nln = grid[static_cast<size_t>(load)].nodes().size();
  for (size_t j = 0; j < nln; ++j) {
    const size_t g = nid(load, j);
    mesh.nodes()[j].setVoltageV(fix[g] ? vdd : vall[g]);
  }

  if (stats != nullptr) {
    stats->layers.clear();
    stats->vias.clear();
    stats->source_layer = src;
    stats->load_layer = load;
    stats->total_nodes = static_cast<int>(nt);
    stats->fixed_nodes = n_fix;
    stats->unknown_nodes = m;
    stats->isolated_nodes = 0;
    stats->isolated_loaded_nodes = 0;
    stats->isolated_fixed_nodes = 0;
    for (int li = 0; li < L; ++li) {
      const size_t b0 = pfx[static_cast<size_t>(li)];
      const size_t nn = grid[static_cast<size_t>(li)].nodes().size();
      double sv = 0.0;
      int nu = 0;
      for (size_t j = 0; j < nn; ++j) {
        const size_t g = b0 + j;
        sv += vall[g];
        if (!fix[g])
          ++nu;
      }
      MultiLayerSolveStats::LayerInfo q;
      q.name = metal[static_cast<size_t>(li)].name;
      q.horizontal = metal[static_cast<size_t>(li)].horiz;
      q.g_per_segment = metal[static_cast<size_t>(li)].g;
      q.rank = metal[static_cast<size_t>(li)].tier;
      q.avg_v = nn ? sv / static_cast<double>(nn) : vdd;
      q.unknown_nodes = nu;
      q.node_count = static_cast<int>(nn);
      q.pitch_um = li == load ? mesh.pitchXUm() : metal[static_cast<size_t>(li)].pitch;
      stats->layers.push_back(move(q));
    }
    for (const auto& e : pairMetal) {
      MultiLayerSolveStats::ViaInfo v;
      v.from_layer = e.first;
      v.to_layer = e.second;
      v.g_via = g_via;
      stats->vias.push_back(v);
    }
  }
}


void SoftKnapsackProfile::rebuildFromItemPairs(vector<pair<double, double>> items) {
  sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
    return (a.first / a.second) > (b.first / b.second);
  });
  _currentPerInstance.clear();
  _areaPerInstance.clear();
  for (const auto& pr : items) {
    _currentPerInstance.push_back(pr.first);
    _areaPerInstance.push_back(pr.second);
  }
  const size_t m = _currentPerInstance.size();
  _prefixArea.assign(m + 1, 0.0);
  _prefixCurrent.assign(m + 1, 0.0);
  for (size_t j = 0; j < m; ++j) {
    _prefixArea[j + 1] = _prefixArea[j] + _areaPerInstance[j];
    _prefixCurrent[j + 1] = _prefixCurrent[j] + _currentPerInstance[j];
  }
}

double SoftKnapsackProfile::maxCurrentForAreaBudget(double W) const {
  if (W <= 0.0 || _currentPerInstance.empty())
    return 0.0;
  auto it = upper_bound(_prefixArea.begin(), _prefixArea.end(), W);
  const size_t i = static_cast<size_t>(it - _prefixArea.begin());
  const size_t m = _currentPerInstance.size();
  const size_t full = (i == 0) ? 0 : i - 1;
  double out = _prefixCurrent[full];
  if (full < m && W > _prefixArea[full])
    out += (W - _prefixArea[full]) / _areaPerInstance[full] * _currentPerInstance[full];
  return out;
}

IrModel::IrModel(const Chip& chip) : _chip(&chip) {}

void IrModel::setStrapSheetModel(const StrapSheetModel& strap) {
  _strapModel = strap;
}

/// Builds the primary mesh on `layout.core()`; this grid is the **load-layer** lattice used for
/// exports, soft/hard load placement, and (when using per-layer solve) must match the load metal's
/// strap pitch from `deriveMeshPlan`.
void IrModel::buildUniformMesh(double pitch_x_um, double pitch_y_um) {
  _mesh = make_mesh(_chip->layout(), pitch_x_um, pitch_y_um, _estimationOptions.supplyVoltageV());
}

void IrModel::loadHardMacroCurrents() {
  _hardMacros = hard_currents(*_chip, _estimationOptions);
}

void IrModel::analyzeSoftModules() {
  _softModuleData = analyze_soft_modules(*_chip, _estimationOptions);
}

void IrModel::assignMeshLoads() {
  assign_mesh_loads(*_chip, _chip->layout(), _estimationOptions, _mesh, _hardMacros, _softModuleData);
}

/// Runs `solve_pg_mesh_dc` with PSM rectangles from the manifest; results update `_mesh` node
/// voltages on the load layer.
void IrModel::solvePgMeshDc(double v_supply_V, int max_cg_iter, double cg_tol_rel) {
  if (_pdnModel.layers.empty())
    throw runtime_error("pdn_ir: IrModel missing PDN model; call setPdnModel() before solve");
  const vector<Rectangle> rects = psm_supply_source_rects_um(*_chip);
  solve_pg_mesh_dc(_mesh,
                     _strapModel,
                     _pdnModel,
                     rects,
                     v_supply_V,
                     max_cg_iter,
                     cg_tol_rel,
                     _chip->irVsrcCenterNodeOnly(),
                     *_chip,
                     &_hardMacros,
                     &_hardPinGraphAttach,
                     &_mlStats,
                     &_pdnGraphNodes,
                     &_pdnGraphEdges);
}

double IrModel::hardPinVoltage(double pin_x_um, double pin_y_um, double current_A) const {
  return hard_pin_V(_mesh, _strapModel, pin_x_um, pin_y_um, current_A);
}

double IrModel::softClusterWorstVoltage(const FpBox& module_box,
                                        const SoftKnapsackProfile& kn) const {
  return soft_tile_worst_V(_mesh, _strapModel, module_box, kn, _chip->layout());
}

double IrModel::softClusterWorstVoltage(const FpBox& module_box,
                                        const string& cluster_name) const {
  auto it = _softModuleData.knapsackByClusterName().find(cluster_name);
  if (it == _softModuleData.knapsackByClusterName().end())
    return numeric_limits<double>::quiet_NaN();
  return soft_tile_worst_V(_mesh, _strapModel, module_box, it->second, _chip->layout());
}

}  // namespace phys
