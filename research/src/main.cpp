#include <phys/chip.hpp>
#include <phys/pdn_ir.hpp>

#include <filesystem>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

int main(int argc, char** argv) {
  try {
    std::filesystem::path manifest = "mempool.json";
    std::string mesh_mode = "synth";  // synth | odb | multilayer
    std::filesystem::path pdn_csv;
    std::string ir_mode_str = "max";  // max (paper, Gx=i) | sum (legacy lumped)
    double via_r_ohm = 0.0;

    auto print_usage = [&]() {
      std::cerr << "usage: " << (argc >= 1 ? argv[0] : "phys_load")
                << " [manifest.json]"
                << " [--mesh synth|odb|multilayer]"
                << " [--pdn-csv <path>]"
                << " [--ir-mode sum|max]"
                << " [--via-r <ohm>]\n";
    };

    {
      int i = 1;
      if (i < argc && argv[i][0] != '-') {
        manifest = argv[i++];
      }
      for (; i < argc; ++i) {
        std::string a = argv[i];
        auto need_val = [&](const char* name) -> std::string {
          if (i + 1 >= argc)
            throw std::runtime_error(std::string(name) + " requires a value");
          return std::string(argv[++i]);
        };
        if (a == "--mesh") {
          mesh_mode = need_val("--mesh");
          if (mesh_mode != "synth" && mesh_mode != "odb"
              && mesh_mode != "multilayer")
            throw std::runtime_error("--mesh must be synth, odb, or multilayer");
        } else if (a == "--pdn-csv") {
          pdn_csv = need_val("--pdn-csv");
        } else if (a == "--ir-mode") {
          ir_mode_str = need_val("--ir-mode");
          if (ir_mode_str != "sum" && ir_mode_str != "max")
            throw std::runtime_error("--ir-mode must be sum or max");
        } else if (a == "--via-r") {
          via_r_ohm = std::stod(need_val("--via-r"));
        } else if (a == "-h" || a == "--help") {
          print_usage();
          return 0;
        } else {
          print_usage();
          throw std::runtime_error("unknown arg: " + a);
        }
      }
    }

    if (!std::filesystem::is_regular_file(manifest)) {
      print_usage();
      return 1;
    }
    if ((mesh_mode == "odb" || mesh_mode == "multilayer") && pdn_csv.empty())
      throw std::runtime_error(
          "--mesh odb/multilayer requires --pdn-csv <path> (produced by "
          "flow/scripts/dump_pdn_mesh.tcl).");

    phys::Chip c = phys::load_chip(manifest);

    std::cout
        << "--- phys_load summary ---\n"
        << "What each block means:\n"
        << "  layout     Die/core rectangles (um) from manifest; odb/def paths if set.\n"
        << "  kit        Paths to LEF/LIB files (not parsed).\n"
        << "  rc/tracks  From setRC.tcl / make_tracks.tcl (extraction + routing grid).\n"
        << "  pdn        From grid_strategy Tcl: straps, inter-layer connects, grids, "
           "voltage\n"
        << "             domains, and global-connection rule count (early floorplan script).\n"
        << "  rtlmp      Instance->cluster id map + root.fp.txt cluster/macro boxes (um).\n"
        << "  pwr        Optional per-instance power table from manifest.\n\n";

    std::cout << "[design] " << c.name << "\n";

    std::cout << "[layout] die llx,lly,urx,ury (um): " << c.layout.die[0] << ", "
              << c.layout.die[1] << ", " << c.layout.die[2] << ", " << c.layout.die[3]
              << "\n";
    std::cout << "[layout] core (um): " << c.layout.core[0] << ", " << c.layout.core[1]
              << ", " << c.layout.core[2] << ", " << c.layout.core[3] << "\n";

    std::cout << "[kit] tech LEF: " << c.kit.tech_lef << "\n";

    std::cout << "[rc] " << c.tech.rc.size()
              << " metal layers in set_layer_rc (R and C per layer; OpenROAD units in Tcl).\n";

    std::cout << "[tracks] " << c.tech.tracks.size()
              << " routing layers in make_tracks (offsets/pitches, um).\n";
    std::cout << "[rc] default signal net RC layer: " << c.tech.sig_layer
              << "; clock net RC layer: " << c.tech.clk_layer << "\n";

    std::cout << "[pdn] global_connect called in script: "
              << (c.tech.global_connect_called ? "yes" : "no")
              << " (runs after add_global_connection rules).\n";

    std::cout << "[pdn] add_global_connection: " << c.tech.pg_conn.size()
              << " rules (stored in Chip; details rarely needed ??? see pdn Tcl).\n";

    std::cout << "[pdn] voltage domains: " << c.tech.vdomains.size() << "\n";
    for (const auto& d : c.tech.vdomains) {
      std::cout << "      " << d.name << "  power=" << d.power_net
                << "  ground=" << d.gnd_net << "\n";
    }

    std::cout << "[pdn] define_pdn_grid: " << c.tech.pdn_grids.size() << " grids\n";
    for (const auto& g : c.tech.pdn_grids) {
      std::cout << "      grid \"" << g.name << "\"";
      if (!g.voltage_domains.empty()) {
        std::cout << "  domains:";
        for (const auto& v : g.voltage_domains)
          std::cout << ' ' << v;
      }
      if (!g.pins.empty()) {
        std::cout << "  pins:";
        for (const auto& p : g.pins)
          std::cout << ' ' << p;
      }
      if (g.macro)
        std::cout << "  [macro grid]";
      if (!g.orient.empty())
        std::cout << "  orient=" << g.orient;
      if (g.has_halo) {
        std::cout << "  halo=" << g.halo[0] << ',' << g.halo[1] << ',' << g.halo[2] << ','
                  << g.halo[3];
      }
      if (g.is_default)
        std::cout << "  [default]";
      std::cout << "\n";
    }

    std::cout << "[pdn] add_pdn_connect: " << c.tech.pdn_connects.size()
              << " layer pairs (via stacks between straps).\n";
    for (const auto& l : c.tech.pdn_connects) {
      std::cout << "      grid " << l.grid << "  " << l.layer_lower << " -> "
                << l.layer_upper << "\n";
    }

    std::cout << "[pdn] add_pdn_stripe: " << c.tech.stripes.size()
              << " straps (width/pitch/offset um).\n";
    for (const auto& s : c.tech.stripes) {
      std::cout << "      grid " << s.grid << "  layer " << s.layer << "  w=" << s.width
                << "  pitch=" << s.pitch << "  off=" << s.offset;
      if (s.followpins)
        std::cout << "  [followpins]";
      std::cout << "\n";
    }

    size_t manual_power_cnt = 0;
    size_t sta_power_cnt = 0;
    size_t cluster_cnt = 0;
    std::unordered_set<int> distinct_clusters;
    for (const auto& kv : c.instances) {
      const auto& inst = kv.second;
      if (inst.has_manual_power)
        ++manual_power_cnt;
      if (inst.has_sta_power)
        ++sta_power_cnt;
      if (inst.has_cluster) {
        ++cluster_cnt;
        distinct_clusters.insert(inst.cluster_id);
      }
    }
    std::cout << "[inst] " << c.instances.size() << " total instances in unified table\n";
    std::cout << "[pwr] " << manual_power_cnt << " manual power entries in manifest\n";
    std::cout << "[pwr] " << sta_power_cnt
              << " per-instance rows loaded from report_power_instances.tsv\n";

    std::cout << "[rtlmp] membership file: " << cluster_cnt
              << " instances carry a cluster id; " << distinct_clusters.size()
              << " distinct ids (RTLMP leaf groups).\n";
    std::cout << "[rtlmp] root.fp.txt: " << c.fp.size()
              << " named rectangles (clusters/macros/etc., um).\n";

    if (cluster_cnt != 0 && sta_power_cnt != 0) {
      size_t joined = 0;
      for (const auto& kv : c.instances) {
        const auto& inst = kv.second;
        if (inst.has_cluster && inst.has_sta_power)
          ++joined;
      }
      std::cout << "[join] power??�cluster: " << joined << " instances"
                << " (power coverage by cluster map: "
                << (100.0 * static_cast<double>(joined) / static_cast<double>(sta_power_cnt))
                << "%, cluster coverage by power map: "
                << (100.0 * static_cast<double>(joined) / static_cast<double>(cluster_cnt))
                << "%)\n";
    }

    size_t area_cnt = 0;
    size_t loc_cnt = 0;
    size_t pg_pin_point_cnt = 0;
    for (const auto& kv : c.instances) {
      const auto& inst = kv.second;
      if (inst.has_area)
        ++area_cnt;
      if (inst.has_loc)
        ++loc_cnt;
      pg_pin_point_cnt += inst.pg_pins.size();
    }
    std::cout << "[geom] instance area rows: " << area_cnt << ", location rows: " << loc_cnt
              << ", pg pin points: " << pg_pin_point_cnt << "\n";

    auto infer_vdd_from_power_rows = [&](const phys::Chip& chip) {
      std::vector<double> ratios;
      ratios.reserve(chip.instances.size());
      for (const auto& kv : chip.instances) {
        const auto& inst = kv.second;
        if (!inst.has_sta_power)
          continue;
        if (!inst.sta_power.has_current)
          continue;
        if (inst.sta_power.current_A <= 1e-15 || inst.sta_power.total_W <= 1e-15)
          continue;
        ratios.push_back(inst.sta_power.total_W / inst.sta_power.current_A);
      }
      if (ratios.empty()) {
        throw std::runtime_error(
            "Cannot infer VDD from report_power_instances.tsv: no valid total_W/current_A rows.");
      }
      std::sort(ratios.begin(), ratios.end());
      return ratios[ratios.size() / 2];
    };

    struct MeshIrFromOpenroad {
      double pitch_x_um{};
      double pitch_y_um{};
      double w_h_um{};
      double w_v_um{};
      double r_sqh{};
      double r_sqv{};
      std::string layer_h;
      std::string layer_v;
      std::string source;
    };

    auto derive_mesh_ir_from_openroad = [&](const phys::Chip& chip) -> MeshIrFromOpenroad {
      std::unordered_map<std::string, double> rc_r_per_um;
      for (const auto& rc : chip.tech.rc) {
        rc_r_per_um[rc.layer] = rc.r;
      }

      struct StripePick {
        std::string layer;
        double width{};
        double pitch{};
      };
      std::vector<StripePick> picks;
      for (const auto& s : chip.tech.stripes) {
        // Prefer core grid straps (exclude followpins rail; exclude macro-only grids).
        if (s.followpins)
          continue;
        if (!s.grid.empty() && s.grid != "grid")
          continue;
        if (s.width <= 0.0 || s.pitch <= 0.0)
          continue;
        picks.push_back({s.layer, s.width, s.pitch});
      }
      if (picks.empty()) {
        throw std::runtime_error(
            "No usable add_pdn_stripe entries found in pdn_tcl to derive mesh pitch/width.");
      }

      std::sort(picks.begin(),
                picks.end(),
                [](const StripePick& a, const StripePick& b) { return a.pitch < b.pitch; });
      const StripePick v = picks[0];
      const StripePick h = picks.size() >= 2 ? picks[1] : picks[0];

      auto get_r = [&](const std::string& layer) {
        auto it = rc_r_per_um.find(layer);
        if (it == rc_r_per_um.end()) {
          throw std::runtime_error("Missing set_layer_rc for layer " + layer
                                   + " referenced by add_pdn_stripe.");
        }
        return it->second;
      };

      MeshIrFromOpenroad out;
      out.pitch_x_um = h.pitch;
      out.pitch_y_um = v.pitch;
      out.w_h_um = h.width;
      out.w_v_um = v.width;
      out.layer_h = h.layer;
      out.layer_v = v.layer;
      // Convert R(ohm/um) to equivalent sheet resistance for requested formula.
      out.r_sqh = get_r(h.layer) * h.width;
      out.r_sqv = get_r(v.layer) * v.width;
      out.source = "pdn_tcl(add_pdn_stripe) + setRC.tcl(set_layer_rc)";
      return out;
    };

    auto infer_cell_area_fallback = [&](const phys::Chip& chip) {
      std::vector<double> vals;
      vals.reserve(chip.instances.size());
      for (const auto& kv : chip.instances) {
        const auto& inst = kv.second;
        if (inst.has_area && inst.area_um2 > 0.0)
          vals.push_back(inst.area_um2);
      }
      if (vals.empty())
        throw std::runtime_error("No positive instance areas available to infer fallback cell area.");
      std::sort(vals.begin(), vals.end());
      return vals[vals.size() / 2];
    };

    auto infer_packing_util = [&](const phys::Chip& chip) {
      double sum_area = 0.0;
      for (const auto& kv : chip.instances) {
        const auto& inst = kv.second;
        if (inst.has_area && inst.area_um2 > 0.0)
          sum_area += inst.area_um2;
      }
      const double core_w = chip.layout.core[2] - chip.layout.core[0];
      const double core_h = chip.layout.core[3] - chip.layout.core[1];
      const double core_area = core_w * core_h;
      if (core_area <= 0.0)
        throw std::runtime_error("Invalid core area in manifest.");
      double util = sum_area / core_area;
      if (util <= 0.0 || !std::isfinite(util))
        throw std::runtime_error("Cannot infer packing utilization from instance areas.");
      if (util > 1.0)
        util = 1.0;
      return util;
    };

    const double inferred_vdd = infer_vdd_from_power_rows(c);
    const auto mesh_ir = derive_mesh_ir_from_openroad(c);
    const double inferred_cell_area = infer_cell_area_fallback(c);
    const double inferred_pack_util = infer_packing_util(c);

    phys::IrModel ir(c);
    ir.estOptions().vdd_V = inferred_vdd;
    ir.estOptions().prefer_sta_current = true;
    ir.estOptions().assumed_cell_area_um2 = inferred_cell_area;
    ir.estOptions().packing_utilization = inferred_pack_util;
    ir.estOptions().ir_mode
        = (ir_mode_str == "max") ? phys::IrMode::Max : phys::IrMode::Sum;
    ir.estOptions().via_r_ohm = via_r_ohm;
    ir.setStrapSheet(
        {mesh_ir.r_sqh, mesh_ir.r_sqv, mesh_ir.w_h_um, mesh_ir.w_v_um});

    phys::PdnDump pdn_dump;
    if (mesh_mode == "synth") {
      ir.buildUniformMesh(mesh_ir.pitch_x_um, mesh_ir.pitch_y_um);
    } else {
      pdn_dump = phys::load_pdn_dump(pdn_csv);
      if (pdn_dump.segs.empty())
        throw std::runtime_error(
            "pdn dump csv has no segments: " + pdn_csv.string());
      std::unordered_map<std::string, double> rc_r_per_um;
      for (const auto& rc : c.tech.rc)
        rc_r_per_um[rc.layer] = rc.r;
      if (mesh_mode == "multilayer")
        ir.buildMultiLayerMesh(pdn_dump, rc_r_per_um, ir.estOptions().vdd_V);
      else
        ir.buildMeshFromPdnDump(pdn_dump, rc_r_per_um, ir.estOptions().vdd_V);
    }
    ir.loadHardMacroCurrents();
    ir.analyzeSoftModules();
    ir.assignMeshLoads();
    ir.updateMeshVoltages();

    const auto& hard = ir.hardMacros();
    const phys::SoftIrData& soft_data = ir.softData();

    // Select the right node list for summary / CSV output.
    const std::vector<phys::MeshNode>& all_nodes =
        ir.isMultiLayer() ? ir.multiMesh().nodes : ir.mesh().nodes;
    const std::vector<size_t>& all_ring_nodes =
        ir.isMultiLayer() ? ir.multiMesh().ring_nodes : ir.mesh().ring_nodes;

    if (ir.isMultiLayer()) {
      const auto& ml = ir.multiMesh();
      std::cout << "[ir] multi-layer mesh: " << ml.nodes.size()
                << " total nodes, " << ml.layers.size() << " layers";
      for (const auto& li : ml.layers)
        std::cout << "  " << li.name << "(" << li.nx << "x" << li.ny << ")";
      std::cout << "  mode=" << mesh_mode
                << "  ir=" << ir_mode_str
                << "  via_r=" << via_r_ohm << " ohm\n";
      std::cout << "[ir] ring/BTERM nodes: " << ml.ring_nodes.size()
                << "  pin_layer=" << ml.pin_layer
                << "  soft_layer=" << ml.layers[static_cast<size_t>(ml.soft_layer_idx)].name
                << "  hard_layer=" << ml.layers[static_cast<size_t>(ml.hard_layer_idx)].name
                << "\n";
    } else {
      const phys::UniformMesh& mesh = ir.mesh();
      std::cout << "[ir] mesh nodes: " << mesh.nodes.size() << " (" << mesh.nx
                << "x" << mesh.ny << "), pitch=(" << mesh.pitch_x_um << ","
                << mesh.pitch_y_um << ") um  mode=" << mesh_mode
                << "  ir=" << ir_mode_str << "\n";
      std::cout << "[ir] ring nodes: " << mesh.ring_nodes.size() << "\n";
      const phys::StrapSheetModel& strap = ir.strapSheet();
      std::cout << "[ir] model r_sqh=" << strap.r_sqh << " r_sqv=" << strap.r_sqv
                << " w_hstrap=" << strap.w_hstrap_um
                << " w_vstrap=" << strap.w_vstrap_um << " um\n";
    }
    std::cout << "[ir] layers h=" << mesh_ir.layer_h << " v=" << mesh_ir.layer_v
              << " source=" << mesh_ir.source << "\n";
    std::cout << "[ir] inferred VDD=" << ir.estOptions().vdd_V
              << " V, median cell area="
              << ir.estOptions().assumed_cell_area_um2
              << " um^2, inferred packing util="
              << ir.estOptions().packing_utilization << "\n";
    {
      double vmin = std::numeric_limits<double>::infinity();
      double vmax = -std::numeric_limits<double>::infinity();
      for (const auto& n : all_nodes) {
        vmin = std::min(vmin, n.v_V);
        vmax = std::max(vmax, n.v_V);
      }
      if (std::isfinite(vmin))
        std::cout << "[pg] lumped IR (paper): V_min=" << vmin
                  << " V  V_max=" << vmax
                  << " V  drop_max=" << (ir.estOptions().vdd_V - vmin)
                  << " V\n";
    }

    std::filesystem::path out_dir = manifest.parent_path() / "out";
    std::filesystem::create_directories(out_dir);
    const std::filesystem::path hard_csv = out_dir / "ir_hard_macros.csv";
    const std::filesystem::path soft_csv = out_dir / "ir_soft_modules.csv";
    const std::filesystem::path mesh_csv = out_dir / "ir_mesh_nodes.csv";
    const std::filesystem::path via_csv  = out_dir / "ir_via_connections.csv";
    const std::filesystem::path stripe_csv = out_dir / "ir_pdn_stripes.csv";

    {
      std::ofstream f(hard_csv);
      f << "instance\tfp_region\tpg_pin_name\tpin_x_um\tpin_y_um\timax_A\tvpin_est_V\n";
      for (const auto& h : hard) {
        const double vj = ir.hardPinVoltage(h.pin_x_um, h.pin_y_um, h.current_A);
        f << h.instance << '\t' << h.fp_region << '\t' << h.pg_pin_name << '\t' << h.pin_x_um << '\t'
          << h.pin_y_um << '\t' << h.current_A << '\t' << vj << '\n';
      }
    }

    {
      std::ofstream f(soft_csv);
      f << "cluster_id\tcluster_name\tinstance_count\ti_observed_A\ti_worst_box_A\ti_mesh_sum_A\t"
           "vmin_tile_V\n";
      for (const auto& s : soft_data.modules) {
        const phys::FpBox* box = nullptr;
        for (const auto& b : c.fp) {
          if (b.name == s.cluster_name) {
            box = &b;
            break;
          }
        }
        double vk = std::numeric_limits<double>::quiet_NaN();
        auto kit = soft_data.knapsack_by_name.find(s.cluster_name);
        if (box && kit != soft_data.knapsack_by_name.end())
          vk = ir.softClusterWorstVoltage(*box, kit->second);
        f << s.cluster_id << '\t' << s.cluster_name << '\t' << s.instance_count << '\t'
          << s.observed_total_A << '\t' << s.worst_case_A << '\t' << s.i_mesh_sum_A << '\t' << vk
          << '\n';
      }
    }

    {
      std::ofstream f(mesh_csv);
      f << "layer\tix\tiy\tx_um\ty_um\ti_soft_A\ti_hard_A\ti_total_A\tv_V\tis_vsrc\n";
      for (const auto& n : all_nodes) {
        f << n.layer_name << '\t'
          << n.ix << '\t' << n.iy << '\t' << n.x_um << '\t' << n.y_um << '\t'
          << n.I_soft_A << '\t' << n.I_hard_A << '\t'
          << (n.I_soft_A + n.I_hard_A) << '\t' << n.v_V << '\t'
          << (n.is_ring ? 1 : 0) << '\n';
      }
    }

    // Via connections: emit inter-layer edges from the multi-layer adjacency list.
    {
      std::ofstream f(via_csv);
      f << "node_a\tlayer_a\tx_a_um\ty_a_um\tnode_b\tlayer_b\tx_b_um\ty_b_um\tg_siemens\n";
      size_t via_count = 0;
      if (ir.isMultiLayer()) {
        const auto& ml = ir.multiMesh();
        for (size_t i = 0; i < ml.adj.size(); ++i) {
          for (const auto& [j, g] : ml.adj[i]) {
            if (j <= i) continue;  // emit each edge once
            if (ml.nodes[i].layer_idx == ml.nodes[j].layer_idx) continue;
            f << i << '\t' << ml.nodes[i].layer_name << '\t'
              << ml.nodes[i].x_um << '\t' << ml.nodes[i].y_um << '\t'
              << j << '\t' << ml.nodes[j].layer_name << '\t'
              << ml.nodes[j].x_um << '\t' << ml.nodes[j].y_um << '\t'
              << g << '\n';
            ++via_count;
          }
        }
      }
      std::cout << "[via] via_connections=" << via_count << "  output=" << via_csv << "\n";
    }

    // PDN stripes: copy loaded PDN dump segments to the output directory.
    {
      std::ofstream f(stripe_csv);
      f << "kind\tlayer\txlo_um\tylo_um\txhi_um\tyhi_um\n";
      for (const auto& s : pdn_dump.segs) {
        f << s.kind << '\t' << s.layer << '\t'
          << s.xlo << '\t' << s.ylo << '\t' << s.xhi << '\t' << s.yhi << '\n';
      }
      std::cout << "[pdn] stripe_segments=" << pdn_dump.segs.size()
                << "  output=" << stripe_csv << "\n";
    }

    std::cout << "[hard] pin_rows=" << hard.size() << "  output=" << hard_csv << "\n";

    std::map<std::string, std::vector<const phys::HardMacroCurrent*>> pins_by_instance;
    for (const auto& h : hard)
      pins_by_instance[h.instance].push_back(&h);

    std::map<std::string, int> fp_region_ix;
    {
      std::set<std::string> reg_keys;
      for (const auto& h : hard)
        reg_keys.insert(h.fp_region);
      int r = 0;
      for (const auto& s : reg_keys)
        fp_region_ix.emplace(s, r++);
    }

    auto macro_index = [&](const std::string& inst) -> int {
      int i = 0;
      for (const auto& kv : pins_by_instance) {
        if (kv.first == inst)
          return i;
        ++i;
      }
      return -1;
    };

    if (!pins_by_instance.empty()) {
      std::cout << "[hard-macro] " << pins_by_instance.size()
                << " hard macro instance(s); macro index = sorted instance name, fp_region = sorted "
                   "unique RTLMP box string:\n";
      std::cout << std::fixed << std::setprecision(3);
      int midx = 0;
      for (const auto& kv : pins_by_instance) {
        const auto& plist = kv.second;
        const int rix = fp_region_ix.at(plist.front()->fp_region);
        std::cout << "      [" << midx << "] n_pins=" << plist.size() << "  fp_region=[" << rix
                  << "]\n";
        for (const phys::HardMacroCurrent* p : plist) {
          const std::string& nm = p->pg_pin_name;
          std::cout << "        " << (nm.empty() ? "(unnamed)" : nm) << "  (" << p->pin_x_um << ", "
                    << p->pin_y_um << ") um\n";
        }
        ++midx;
      }
      std::cout << std::defaultfloat << std::setprecision(6);
    }

    std::cout << "[soft] count=" << soft_data.modules.size() << "  output=" << soft_csv << "\n";
    std::cout << "[mesh] nodes=" << all_nodes.size() << "  output=" << mesh_csv << "\n";
    if (!hard.empty()) {
      const auto& h = hard.front();
      const double vj = ir.hardPinVoltage(h.pin_x_um, h.pin_y_um, h.current_A);
      const int hi = macro_index(h.instance);
      const int ri = fp_region_ix.at(h.fp_region);
      std::cout << "[hard-top] macro=[" << hi << "] fp_region=[" << ri << "] Imax_share=" << h.current_A
                << " A  Vpin_est=" << vj << " V  pin="
                << (h.pg_pin_name.empty() ? "?" : h.pg_pin_name) << "\n";
    }
    if (!soft_data.modules.empty()) {
      const auto& s = soft_data.modules.front();
      std::cout << "[soft-top] Iobs=" << s.observed_total_A << " A  Ibox~=" << s.worst_case_A
                << " A  Imesh_sum=" << s.i_mesh_sum_A << " A  cid=" << s.cluster_id << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
