// phys_load: read manifest + exported Tcl/TSV context, build a coarse PDN mesh IR model, and
// write summary TSVs under manifest/out/. Mesh pitch for loads follows the lowest metal PDN strap
// layer (same convention as the multi-layer DC solver in pdn_ir.cpp).
#include <phys/chip.hpp>
#include <phys/pdn_ir.hpp>
#include <phys/pdn_model.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

using namespace std;
namespace fs = std::filesystem;

namespace {

/// Loads `Chip`, derives a core mesh from PDN strap data, runs IR estimation hooks, DC mesh solve,
/// and TSV export for downstream scripts.
class PhysLoadReporter {
 public:
  explicit PhysLoadReporter(fs::path manifest_path)
      : _manifestPath(move(manifest_path)) {}

  /// Entry point: build model, solve, print summaries, write `out/*.tsv`.
  int run();

 private:
  struct MeshPlan {
    phys::StrapSheetModel strap_model;
    double pitch_x_um{};
    double pitch_y_um{};
    string layer_h;
    string layer_v;
  };

  static double inferVddFromPowerRows(const phys::Chip& chip);
  static double resolveSupplyVoltageV(const phys::Chip& chip);
  static MeshPlan deriveMeshPlan(const phys::PdnModel& pdn);
  static double inferMedianCellAreaUm2(const phys::Chip& chip);
  static double inferPackingUtilization(const phys::Chip& chip);

  void printDesignSummary(const phys::Chip& chip) const;
  void printIrSummary(const phys::IrModel& ir,
                      const string& layer_h,
                      const string& layer_v,
                      const string& mesh_source) const;
  void writeIrTsvOutputs(const phys::Chip& chip,
                         const phys::IrModel& ir,
                         const phys::UniformMesh& mesh) const;

  fs::path _manifestPath;
};

double PhysLoadReporter::inferVddFromPowerRows(const phys::Chip& chip) {
  vector<double> ratios;
  ratios.reserve(chip.instances().size());
  for (const auto& kv : chip.instances()) {
    const auto& inst = kv.second;
    if (!inst.hasStaPower())
      continue;
    const auto& sp = inst.staPower();
    if (!sp.has_current)
      continue;
    if (sp.current_A <= 1e-15 || sp.total_W <= 1e-15)
      continue;
    ratios.push_back(sp.total_W / sp.current_A);
  }
  if (ratios.empty()) {
    throw runtime_error(
        "Cannot infer VDD from report_power_instances.tsv: no valid total_W/current_A rows.");
  }
  sort(ratios.begin(), ratios.end());
  return ratios[ratios.size() / 2];
}

double PhysLoadReporter::resolveSupplyVoltageV(const phys::Chip& chip) {
  if (auto v = phys::flow_supply_voltage_V(chip)) {
    cout << "[vdd] " << *v
         << " V (flow config.mk PWR_NETS_VOLTAGES — same net list as final_report.tcl "
            "IR drop)\n";
    return *v;
  }
  const double inferred = inferVddFromPowerRows(chip);
  cout << "[vdd] " << inferred
       << " V (median total_W/current_A from report_power_instances.tsv; "
          "override flow/platforms/.../config.mk or flow/designs/.../config.mk "
          "PWR_NETS_VOLTAGES to use OpenROAD supply)\n";
  return inferred;
}

/// Chooses horizontal/vertical strap sheet R from the two finest-pitch stripes (after RC filter),
/// and sets the **load-layer** uniform mesh pitch: square grid at the strap pitch of the lowest
/// metal-number PDN layer (matches `solve_pg_mesh_dc` load_layer selection).
PhysLoadReporter::MeshPlan PhysLoadReporter::deriveMeshPlan(const phys::PdnModel& pdn) {
  MeshPlan plan;
  if (pdn.layers.empty())
    throw runtime_error("No PDN abstract layers were derived.");
  const auto& v = pdn.layers.front();
  const auto& h = pdn.layers.size() >= 2 ? pdn.layers[1] : pdn.layers.front();

  auto rank_layer = [](const string& name) {
    int out = -1;
    int cur = -1;
    for (char c : name) {
      if (isdigit(static_cast<unsigned char>(c))) {
        if (cur < 0)
          cur = 0;
        cur = cur * 10 + (c - '0');
        out = cur;
      } else {
        cur = -1;
      }
    }
    return out;
  };

  vector<string> layer_names;
  for (const auto& p : pdn.layers)
    layer_names.push_back(p.name);
  int load_li = 0;
  for (int li = 1; li < static_cast<int>(layer_names.size()); ++li) {
    const int rk = rank_layer(layer_names[static_cast<size_t>(li)]);
    const int rload = rank_layer(layer_names[static_cast<size_t>(load_li)]);
    if (rk >= 0 && (rload < 0 || rk < rload))
      load_li = li;
  }
  double load_pitch = pdn.layers.front().pitchUm;
  for (const auto& p : pdn.layers) {
    if (p.name == layer_names[static_cast<size_t>(load_li)]) {
      load_pitch = p.pitchUm;
      break;
    }
  }

  auto get_r = [&](const string& layer) {
    for (const auto& r : pdn.layers)
      if (r.name == layer && r.gSeg > 0.0 && r.pitchUm > 0.0)
        return 1.0 / (r.gSeg * r.pitchUm);
    throw runtime_error("Missing PDN abstract layer " + layer);
  };

  plan.strap_model = phys::StrapSheetModel(get_r(h.name) * h.widthUm, get_r(v.name) * v.widthUm,
                                           h.widthUm, v.widthUm);
  plan.pitch_x_um = load_pitch;
  plan.pitch_y_um = load_pitch;
  plan.layer_h = h.name;
  plan.layer_v = v.name;
  return plan;
}

double PhysLoadReporter::inferMedianCellAreaUm2(const phys::Chip& chip) {
  vector<double> vals;
  vals.reserve(chip.instances().size());
  for (const auto& kv : chip.instances()) {
    const auto& inst = kv.second;
    if (inst.hasArea() && inst.areaUm2() > 0.0)
      vals.push_back(inst.areaUm2());
  }
  if (vals.empty())
    throw runtime_error("No positive instance areas available to infer fallback cell area.");
  sort(vals.begin(), vals.end());
  return vals[vals.size() / 2];
}

double PhysLoadReporter::inferPackingUtilization(const phys::Chip& chip) {
  double sum_area = 0.0;
  for (const auto& kv : chip.instances()) {
    const auto& inst = kv.second;
    if (inst.hasArea() && inst.areaUm2() > 0.0)
      sum_area += inst.areaUm2();
  }
  const auto& core = chip.layout().core();
  const double core_w = core.urx() - core.llx();
  const double core_h = core.ury() - core.lly();
  const double core_area = core_w * core_h;
  if (core_area <= 0.0)
    throw runtime_error("Invalid core area in manifest.");
  double util = sum_area / core_area;
  if (util <= 0.0 || !isfinite(util))
    throw runtime_error("Cannot infer packing utilization from instance areas.");
  if (util > 1.0)
    util = 1.0;
  return util;
}

void PhysLoadReporter::printDesignSummary(const phys::Chip& chip) const {
  cout << "--- phys_load summary ---\n";

  cout << "[design] " << chip.designName() << "\n";

  const auto& die = chip.layout().die();
  cout << "[layout] die llx,lly,urx,ury (um): " << die.llx() << ", " << die.lly() << ", "
            << die.urx() << ", " << die.ury() << "\n";
  const auto& core = chip.layout().core();
  cout << "[layout] core (um): " << core.llx() << ", " << core.lly() << ", " << core.urx()
            << ", " << core.ury() << "\n";

  cout << "[kit] tech LEF: " << chip.kit().techLefPath() << "\n";

  cout << "[rc] " << chip.tech().layerResistanceCapacitance().size()
              << " metal layers in set_layer_rc (R and C per layer; OpenROAD units in Tcl).\n";

  cout << "[tracks] " << chip.tech().trackRules().size()
              << " routing layers in make_tracks (offsets/pitches, um).\n";
  cout << "[rc] default signal net RC layer: " << chip.tech().signalWireRcLayer()
            << "; clock net RC layer: " << chip.tech().clockWireRcLayer() << "\n";

  cout << "[pdn] global_connect called in script: "
            << (chip.tech().globalConnectCalledInScript() ? "yes" : "no")
              << " (runs after add_global_connection rules).\n";

  cout << "[pdn] add_global_connection: " << chip.tech().globalConnections().size()
              << " rules (stored in Chip; details rarely needed — see pdn Tcl).\n";

  cout << "[pdn] voltage domains: " << chip.tech().voltageDomains().size() << "\n";
  for (const auto& d : chip.tech().voltageDomains()) {
    cout << "      " << d.name << "  power=" << d.power_net << "  ground=" << d.gnd_net
              << "\n";
  }

  cout << "[pdn] define_pdn_grid: " << chip.tech().pdnGrids().size() << " grids\n";
  for (const auto& g : chip.tech().pdnGrids()) {
    cout << "      grid \"" << g.name << "\"";
      if (!g.voltage_domains.empty()) {
      cout << "  domains:";
        for (const auto& v : g.voltage_domains)
        cout << ' ' << v;
      }
      if (!g.pins.empty()) {
      cout << "  pins:";
        for (const auto& p : g.pins)
        cout << ' ' << p;
      }
      if (g.macro)
      cout << "  [macro grid]";
      if (!g.orient.empty())
      cout << "  orient=" << g.orient;
      if (g.has_halo) {
      cout << "  halo=" << g.halo[0] << ',' << g.halo[1] << ',' << g.halo[2] << ','
                  << g.halo[3];
      }
      if (g.is_default)
      cout << "  [default]";
    cout << "\n";
    }

  cout << "[pdn] add_pdn_connect: " << chip.tech().pdnConnects().size()
              << " layer pairs (via stacks between straps).\n";
  for (const auto& l : chip.tech().pdnConnects()) {
    cout << "      grid " << l.grid << "  " << l.layer_lower << " -> " << l.layer_upper
              << "\n";
  }

  cout << "[pdn] add_pdn_stripe: " << chip.tech().stripes().size()
              << " straps (width/pitch/offset um).\n";
  for (const auto& s : chip.tech().stripes()) {
    cout << "      grid " << s.grid << "  layer " << s.layer << "  w=" << s.width
                << "  pitch=" << s.pitch << "  off=" << s.offset;
      if (s.followpins)
      cout << "  [followpins]";
    cout << "\n";
    }

    size_t manual_power_cnt = 0;
    size_t sta_power_cnt = 0;
    size_t cluster_cnt = 0;
  unordered_set<int> distinct_clusters;
  for (const auto& kv : chip.instances()) {
      const auto& inst = kv.second;
    if (inst.hasManualPower())
        ++manual_power_cnt;
    if (inst.hasStaPower())
        ++sta_power_cnt;
    if (inst.hasCluster()) {
        ++cluster_cnt;
      distinct_clusters.insert(inst.clusterId());
    }
  }
  cout << "[inst] " << chip.instances().size() << " total instances in unified table\n";
  cout << "[pwr] " << manual_power_cnt << " manual power entries in manifest\n";
  cout << "[pwr] " << sta_power_cnt
              << " per-instance rows loaded from report_power_instances.tsv\n";

  cout << "[rtlmp] membership file: " << cluster_cnt << " instances carry a cluster id; "
            << distinct_clusters.size() << " distinct ids (RTLMP leaf groups).\n";
  cout << "[rtlmp] root.fp.txt: " << chip.floorplanRegions().size()
              << " named rectangles (clusters/macros/etc., um).\n";

    if (cluster_cnt != 0 && sta_power_cnt != 0) {
      size_t joined = 0;
    for (const auto& kv : chip.instances()) {
        const auto& inst = kv.second;
      if (inst.hasCluster() && inst.hasStaPower())
          ++joined;
      }
    cout << "[join] power∩cluster: " << joined << " instances"
                << " (power coverage by cluster map: "
                << (100.0 * static_cast<double>(joined) / static_cast<double>(sta_power_cnt))
                << "%, cluster coverage by power map: "
                << (100.0 * static_cast<double>(joined) / static_cast<double>(cluster_cnt))
                << "%)\n";
    }

    size_t area_cnt = 0;
    size_t loc_cnt = 0;
    size_t pg_pin_point_cnt = 0;
  for (const auto& kv : chip.instances()) {
      const auto& inst = kv.second;
    if (inst.hasArea())
        ++area_cnt;
    if (inst.hasLocation())
        ++loc_cnt;
    pg_pin_point_cnt += inst.pgPins().size();
    }
  cout << "[geom] instance area rows: " << area_cnt << ", location rows: " << loc_cnt
              << ", pg pin points: " << pg_pin_point_cnt << "\n";
}

void PhysLoadReporter::printIrSummary(const phys::IrModel& ir,
                                      const string& layer_h,
                                      const string& layer_v,
                                      const string& mesh_source) const {
  const phys::UniformMesh& mesh = ir.mesh();
  const auto& strap = ir.strapSheetModel();
  const auto& est = ir.estimationOptions();

  cout << "[ir] uniform mesh nodes: " << mesh.nodes().size() << " (" << mesh.countX() << "x"
            << mesh.countY() << "), pitch=(" << mesh.pitchXUm() << "," << mesh.pitchYUm()
            << ") um\n";
  cout << "[ir] model r_sqh=" << strap.sheetResistanceHorizontal()
            << " r_sqv=" << strap.sheetResistanceVertical()
            << " w_hstrap=" << strap.horizontalStrapWidthUm()
            << " w_vstrap=" << strap.verticalStrapWidthUm() << " um\n";
  cout << "[ir] layers h=" << layer_h << " v=" << layer_v << " source=" << mesh_source << "\n";
  cout << "[ir] inferred VDD=" << est.supplyVoltageV()
            << " V, median cell area=" << est.assumedCellAreaUm2()
            << " um^2, inferred packing util=" << est.packingUtilization() << "\n";

  double vmin = numeric_limits<double>::infinity();
  double vmax = -numeric_limits<double>::infinity();
  for (const auto& n : mesh.nodes()) {
    vmin = min(vmin, n.voltageV());
    vmax = max(vmax, n.voltageV());
  }
  if (isfinite(vmin))
    cout << "[pg] DC mesh solve Gx=i: V_min=" << vmin << " V  V_max=" << vmax << " V  drop_max="
              << (est.supplyVoltageV() - vmin) << " V\n";

  const auto& ml = ir.multiLayerStats();
  if (!ml.layers.empty()) {
    cout << "[ml] layers=" << ml.layers.size() << " src_layer=" << ml.source_layer
         << " load_layer=" << ml.load_layer << " total_nodes=" << ml.total_nodes
         << " fixed=" << ml.fixed_nodes << " unknown=" << ml.unknown_nodes << "\n";
    for (size_t i = 0; i < ml.layers.size(); ++i) {
      const auto& L = ml.layers[i];
      cout << "      [" << i << "] " << L.name << " dir=" << (L.horizontal ? "H" : "V")
           << " pitch_um=" << L.pitch_um << " nodes=" << L.node_count << " Gseg=" << L.g_per_segment
           << " rank=" << L.rank << " avgV=" << L.avg_v << " unk=" << L.unknown_nodes << "\n";
    }
    for (const auto& v : ml.vias) {
      cout << "      via " << v.from_layer << " -> " << v.to_layer << " Gvia=" << v.g_via << "\n";
    }
  }
}

void PhysLoadReporter::writeIrTsvOutputs(const phys::Chip& chip,
                                         const phys::IrModel& ir,
                                         const phys::UniformMesh& mesh) const {
  fs::path out_dir = _manifestPath.parent_path() / "out";
  fs::create_directories(out_dir);
  const fs::path hard_tsv = out_dir / "ir_hard_macros.tsv";
  const fs::path soft_tsv = out_dir / "ir_soft_modules.tsv";
  const fs::path mesh_tsv = out_dir / "ir_mesh_nodes.tsv";
  const fs::path model_tsv = out_dir / "ir_pdn_model.tsv";
  const fs::path graph_nodes_tsv = out_dir / "ir_pdn_graph_nodes.tsv";
  const fs::path graph_edges_tsv = out_dir / "ir_pdn_graph_edges.tsv";
  const fs::path hard_attach_tsv = out_dir / "ir_hard_pin_graph_attach.tsv";

    const auto& hard = ir.hardMacros();
  const auto& soft_data = ir.softModuleData();

  {
    ofstream f(hard_tsv);
      f << "instance\tfp_region\tpg_pin_name\tpin_x_um\tpin_y_um\timax_A\tvpin_est_V\n";
      for (const auto& h : hard) {
        const double vj = ir.hardPinVoltage(h.pin_x_um, h.pin_y_um, h.current_A);
        f << h.instance << '\t' << h.fp_region << '\t' << h.pg_pin_name << '\t' << h.pin_x_um << '\t'
          << h.pin_y_um << '\t' << h.current_A << '\t' << vj << '\n';
      }
    }

    {
    ofstream f(soft_tsv);
      f << "cluster_id\tcluster_name\tinstance_count\ti_observed_A\ti_worst_box_A\ti_mesh_sum_A\t"
           "vmin_tile_V\n";
    for (const auto& s : soft_data.modules()) {
        const phys::FpBox* box = nullptr;
      for (const auto& b : chip.floorplanRegions()) {
        if (b.regionName() == s.cluster_name) {
            box = &b;
            break;
          }
        }
      double vk = numeric_limits<double>::quiet_NaN();
      auto kit = soft_data.knapsackByClusterName().find(s.cluster_name);
      if (box && kit != soft_data.knapsackByClusterName().end())
          vk = ir.softClusterWorstVoltage(*box, kit->second);
        f << s.cluster_id << '\t' << s.cluster_name << '\t' << s.instance_count << '\t'
          << s.observed_total_A << '\t' << s.worst_case_A << '\t' << s.i_mesh_sum_A << '\t' << vk
          << '\n';
      }
    }

    {
    ofstream f(mesh_tsv);
      f << "ix\tiy\tx_um\ty_um\ti_soft_A\ti_hard_A\ti_total_A\tv_V\n";
    for (const auto& n : mesh.nodes()) {
      f << n.gridIndexX() << '\t' << n.gridIndexY() << '\t' << n.xUm() << '\t' << n.yUm() << '\t'
        << n.softCurrentA() << '\t' << n.hardCurrentA() << '\t'
        << (n.softCurrentA() + n.hardCurrentA()) << '\t' << n.voltageV() << '\n';
    }
  }

  {
    ofstream f(model_tsv);
    const auto& ml = ir.multiLayerStats();
    f << "kind\tlayer_idx\tlayer_name\tdirection\tg_per_segment\trank\tavg_v\tunknown_nodes\t"
         "source_layer\tload_layer\ttotal_nodes\tfixed_nodes\tunknown_total\tisolated_nodes\t"
         "isolated_loaded_nodes\tisolated_fixed_nodes\tvia_from\tvia_to\tg_via\n";
    for (size_t i = 0; i < ml.layers.size(); ++i) {
      const auto& L = ml.layers[i];
      f << "layer\t" << i << '\t' << L.name << '\t' << (L.horizontal ? "H" : "V") << '\t'
        << L.g_per_segment << '\t' << L.rank << '\t' << L.avg_v << '\t' << L.unknown_nodes << '\t'
        << ml.source_layer << '\t' << ml.load_layer << '\t' << ml.total_nodes << '\t'
        << ml.fixed_nodes << '\t' << ml.unknown_nodes << '\t' << ml.isolated_nodes << '\t'
        << ml.isolated_loaded_nodes << '\t' << ml.isolated_fixed_nodes << "\t\t\t\n";
    }
    for (const auto& v : ml.vias) {
      f << "via\t\t\t\t\t\t\t\t" << ml.source_layer << '\t' << ml.load_layer << '\t'
        << ml.total_nodes << '\t' << ml.fixed_nodes << '\t' << ml.unknown_nodes << '\t'
        << ml.isolated_nodes << '\t' << ml.isolated_loaded_nodes << '\t' << ml.isolated_fixed_nodes << '\t'
        << v.from_layer << '\t' << v.to_layer << '\t' << v.g_via << '\n';
    }
  }

  const auto& gnodes = ir.pdnGraphNodes();
  const auto& gedges = ir.pdnGraphEdges();
  const auto& hard_attach = ir.hardPinGraphAttach();
  if (!gnodes.empty() || !gedges.empty()) {
    {
      ofstream f(graph_nodes_tsv);
      f << "id\tlayer_idx\tlayer_name\tx_um\ty_um\tv_v\tfixed\ti_load_a\n";
      for (const auto& n : gnodes) {
        f << n.id << '\t' << n.layer_idx << '\t' << n.layer_name << '\t' << n.x_um << '\t'
          << n.y_um << '\t' << n.v_v << '\t' << (n.fixed ? 1 : 0) << '\t' << n.i_load_a << '\n';
      }
    }
    {
      ofstream f(graph_edges_tsv);
      f << "u\tv\tkind\tg\n";
      for (const auto& e : gedges)
        f << e.u << '\t' << e.v << '\t' << e.kind << '\t' << e.g << '\n';
    }
    if (!hard_attach.empty()) {
      ofstream f(hard_attach_tsv);
      f << "instance\tfp_region\tpg_pin_name\tpin_x_um\tpin_y_um\timax_A\tmapped_node_id\tmapped_layer\t"
           "mapped_x_um\tmapped_y_um\tmanhattan_um\n";
      for (const auto& r : hard_attach) {
        f << r.instance << '\t' << r.fp_region << '\t' << r.pg_pin_name << '\t' << r.pin_x_um << '\t'
          << r.pin_y_um << '\t' << r.current_A << '\t' << r.node_id << '\t' << r.node_layer << '\t'
          << r.node_x_um << '\t' << r.node_y_um << '\t' << r.manhattan_um << '\n';
      }
    }
  }

  cout << "[hard] pin_rows=" << hard.size() << "  output=" << hard_tsv << "\n";

  map<string, vector<const phys::HardMacroPinCurrent*>> pins_by_instance;
  for (const auto& h : hard)
    pins_by_instance[h.instance].push_back(&h);

  map<string, int> fp_region_ix;
  {
    set<string> reg_keys;
    for (const auto& h : hard)
      reg_keys.insert(h.fp_region);
    int r = 0;
    for (const auto& s : reg_keys)
      fp_region_ix.emplace(s, r++);
  }

  auto macro_index = [&](const string& inst) -> int {
    int i = 0;
    for (const auto& kv : pins_by_instance) {
      if (kv.first == inst)
        return i;
      ++i;
    }
    return -1;
  };

  cout << "[soft] count=" << soft_data.modules().size() << "  output=" << soft_tsv << "\n";
  cout << "[mesh] nodes=" << mesh.nodes().size() << "  output=" << mesh_tsv << "\n";
  cout << "[model] output=" << model_tsv << "\n";
  if (!gnodes.empty() || !gedges.empty()) {
    cout << "[graph] nodes=" << gnodes.size() << " edges=" << gedges.size()
         << "  outputs=" << graph_nodes_tsv << ", " << graph_edges_tsv << "\n";
    if (!hard_attach.empty()) {
      cout << "[graph-hard] pin_attach_rows=" << hard_attach.size()
           << "  output=" << hard_attach_tsv << "\n";
    }
  }
    if (!hard.empty()) {
      const auto& h = hard.front();
      const double vj = ir.hardPinVoltage(h.pin_x_um, h.pin_y_um, h.current_A);
      const int hi = macro_index(h.instance);
      const int ri = fp_region_ix.at(h.fp_region);
    cout << "[hard-top] macro=[" << hi << "] fp_region=[" << ri << "] Imax_share=" << h.current_A
                << " A  Vpin_est=" << vj << " V  pin="
                << (h.pg_pin_name.empty() ? "?" : h.pg_pin_name) << "\n";
    }
  if (!soft_data.modules().empty()) {
    const auto& s = soft_data.modules().front();
    cout << "[soft-top] Iobs=" << s.observed_total_A << " A  Ibox~=" << s.worst_case_A
                << " A  Imesh_sum=" << s.i_mesh_sum_A << " A  cid=" << s.cluster_id << "\n";
    }
}

int PhysLoadReporter::run() {
  // 1) Manifest + Tcl exports -> Chip
  phys::Chip chip = phys::load_chip(_manifestPath);
  printDesignSummary(chip);
  const bool use_pdnsim_style = []() {
    const char* v = std::getenv("PHYS_PDN_MODEL_STYLE");
    return v != nullptr && string(v) == "pdnsim";
  }();
  const phys::PdnModel pdn = use_pdnsim_style ? phys::buildPdnModelPdnsimStyle(chip)
                                              : phys::buildPdnModel(chip);
  cout << "[pdn] model_style=" << (use_pdnsim_style ? "pdnsim" : "default") << "\n";
  cout << "[pdn] model built before solve: layers=" << pdn.layers.size()
       << " via_pairs=" << pdn.viaPairs.size() << "\n";

  // 2) VDD and mesh plan (strap sheet R + load-layer pitch for the uniform mesh)
  const double inferred_vdd = resolveSupplyVoltageV(chip);
  const MeshPlan mesh_plan = deriveMeshPlan(pdn);

  // 3) Heuristics for soft-cluster current allocation when STA rows omit area
  const double inferred_cell_area = inferMedianCellAreaUm2(chip);
  const double inferred_pack_util = inferPackingUtilization(chip);

  // 4) IR model: mesh on core, attach loads, PSM sources, multi-layer DC solve
  phys::IrModel ir(chip);
  ir.estimationOptions().setSupplyVoltageV(inferred_vdd);
  ir.estimationOptions().setPreferStaCurrent(true);
  ir.estimationOptions().setAssumedCellAreaUm2(inferred_cell_area);
  ir.estimationOptions().setPackingUtilization(inferred_pack_util);
  ir.setStrapSheetModel(mesh_plan.strap_model);
  ir.setPdnModel(pdn);

  ir.buildUniformMesh(mesh_plan.pitch_x_um, mesh_plan.pitch_y_um);
  ir.loadHardMacroCurrents();
  ir.analyzeSoftModules();
  ir.assignMeshLoads();

  const bool skip_ir_solve = []() {
    const char* v = std::getenv("PHYS_SKIP_IR_SOLVE");
    return v != nullptr && string(v) == "1";
  }();
  if (skip_ir_solve) {
    cout << "[pdn] PHYS_SKIP_IR_SOLVE=1 -> skip DC solve (model validation mode)\n";
    cout << "[pdn] layers=" << pdn.layers.size() << " via_pairs=" << pdn.viaPairs.size() << "\n";
    for (size_t i = 0; i < pdn.layers.size(); ++i) {
      const auto& L = pdn.layers[i];
      cout << "      [" << i << "] " << L.name << " dir=" << (L.isHoriz ? "H" : "V")
           << " pitch=" << L.pitchUm << "um width=" << L.widthUm << "um Gseg=" << L.gSeg
           << " rank=" << L.rank << "\n";
    }
    for (const auto& p : pdn.viaPairs)
      cout << "      via " << p.first << " -> " << p.second << "\n";
    return 0;
  }

  ir.solvePgMeshDc(ir.estimationOptions().supplyVoltageV());
  cout << "[mesh vdd] PSM sources from manifest (boxes or .loc), clipped to core\n";
  if (chip.irVsrcCenterNodeOnly())
    cout << "[mesh vdd] ir_vsrc_center_node_only=1: one fixed node per source rect\n";

  // 5) Human-readable log + TSV artifacts
  printIrSummary(ir, mesh_plan.layer_h, mesh_plan.layer_v,
                 "pdn_tcl(add_pdn_stripe) + setRC.tcl(set_layer_rc)");
  writeIrTsvOutputs(chip, ir, ir.mesh());

  return 0;
}

}  // namespace

/// CLI: `phys_load [manifest.json]` — default manifest name if omitted.
int main(int argc, char** argv) {
  try {
    fs::path manifest = "mempool.json";
    if (argc >= 2)
      manifest = argv[1];

    if (!fs::is_regular_file(manifest)) {
      cerr << "usage: " << (argc >= 1 ? argv[0] : "phys_load") << " [manifest.json]\n";
      return 1;
    }

    PhysLoadReporter app(manifest);
    return app.run();
  } catch (const exception& e) {
    cerr << e.what() << '\n';
    return 2;
  }
}
