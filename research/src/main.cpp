#include <phys/chip.hpp>

#include <filesystem>
#include <iostream>
#include <unordered_set>

int main(int argc, char** argv) {
  try {
    std::filesystem::path manifest = "mempool.json";
    if (argc >= 2)
      manifest = argv[1];

    if (!std::filesystem::is_regular_file(manifest)) {
      std::cerr << "usage: " << (argc >= 1 ? argv[0] : "phys_load")
                << " [manifest.json]\n";
      return 1;
    }

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
              << " rules (stored in Chip; details rarely needed — see pdn Tcl).\n";

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
      std::cout << "[join] power∩cluster: " << joined << " instances"
                << " (power coverage by cluster map: "
                << (100.0 * static_cast<double>(joined) / static_cast<double>(sta_power_cnt))
                << "%, cluster coverage by power map: "
                << (100.0 * static_cast<double>(joined) / static_cast<double>(cluster_cnt))
                << "%)\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
