#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace phys {

class Layout {
 public:
  double die[4]{};
  double core[4]{};
  double halo[2]{};
  std::string odb;
  std::string def;
};

class Kit {
 public:
  std::string tech_lef;
  std::string sc_lef;
  std::vector<std::string> lef_extra;
  std::vector<std::string> lib;
};

class LayerRc {
 public:
  std::string layer;
  double r{};
  double c{};
};

/// One power/ground strap from add_pdn_stripe (width, pitch, offset in µm).
class Stripe {
 public:
  std::string grid;
  std::string layer;
  double width{};
  double pitch{};
  double offset{};
  bool followpins{};
};

/// add_global_connection: which supply net, instance regex, pin regex, optional -power / -ground.
class PgConn {
 public:
  std::string net;
  std::string inst_pattern;
  std::string pin_pattern;
  /// "power", "ground", or empty (aux pin on supply, e.g. VDDPE).
  std::string rail;
};

/// set_voltage_domain: logical domain name and its VDD / VSS nets.
class VDomain {
 public:
  std::string name;
  std::string power_net;
  std::string gnd_net;
};

/// define_pdn_grid: one PDN grid (std-cell region or macro halo grid).
class PdnGrid {
 public:
  std::string name;
  std::vector<std::string> voltage_domains;
  /// Top-layer pin layer for std grid (e.g. metal7); empty for macro-only grids.
  std::vector<std::string> pins;
  bool macro{};
  std::string orient;
  double halo[4]{};
  bool has_halo{};
  bool is_default{};
};

/// add_pdn_connect: vertical connection between two routing layers inside one grid.
class PdnConnect {
 public:
  std::string grid;
  std::string layer_lower;
  std::string layer_upper;
};

class TrackRule {
 public:
  std::string layer;
  double x0{};
  double xp{};
  double y0{};
  double yp{};
};

/// Process + metal: RC table, routing bounds, optional PDN script parse, tracks, default wire RC layers.
class Tech {
 public:
  int process_nm{};
  std::string site;
  std::string route_min;
  std::string route_max;
  std::string route_clk_min;
  std::string sig_layer;
  std::string clk_layer;
  std::vector<LayerRc> rc;
  std::vector<TrackRule> tracks;

  // Parsed from pdn_tcl (e.g. grid_strategy*.tcl); early floorplan PDN intent.
  bool global_connect_called{};
  std::vector<PgConn> pg_conn;
  std::vector<VDomain> vdomains;
  std::vector<PdnGrid> pdn_grids;
  std::vector<PdnConnect> pdn_connects;
  std::vector<Stripe> stripes;
};

/// Per-instance power breakdown from OpenSTA report_power (-instances).
class InstPower {
 public:
  double internal_W{};
  double switching_W{};
  double leakage_W{};
  double total_W{};
  // Optional; sometimes derived as total_W / VDD.
  double current_A{};
  bool has_current{};
};

/// Unified per-instance data assembled from multiple inputs (groups/power/etc.).
class Instance {
 public:
  std::string name;
  bool has_cluster{};
  int cluster_id{};

  // Optional scalar power from manifest "pwr" entries (W).
  bool has_manual_power{};
  double manual_power_W{};

  // Optional OpenSTA report_power breakdown.
  bool has_sta_power{};
  InstPower sta_power;
};

// RTLMP root.fp.txt: one rect per line (name x y w h in DBU) — same as report.py
class FpBox {
 public:
  std::string name;
  double lx{}, ly{}, ux{}, uy{};
};

class Chip {
 public:
  std::string name;
  Layout layout;
  Kit kit;
  Tech tech;
  /// Canonical instance table (key=name), carrying cluster and power attributes.
  std::unordered_map<std::string, Instance> instances;
  /// Soft macro / cluster / macro rects from RTLMP floorplan text (µm)
  std::vector<FpBox> fp;
};

Chip load_chip(const std::filesystem::path& manifest_path);

}  // namespace phys
