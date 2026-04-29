#ifndef PHYS_CHIP_HPP
#define PHYS_CHIP_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

// phys: stable, user-facing types and functions for this research tool (chip + IR).
// phys::detail: loader/parser implementation; not part of the public contract—keeps
//   ChipLoader and friend access off the main API surface.

namespace phys {

class Chip;
class Tech;

namespace detail {

/// Builds a Chip from manifest.json (invoked by load_chip).
class ChipLoader {
 public:
  static Chip load(const fs::path& manifest_path);

 private:
  static void parsePdnScript(string_view text, Tech& tech);
};

}  // namespace detail

/// Axis-aligned rectangle in micrometers (lower-left inclusive, upper-right convention).
class Rectangle {
 public:
  Rectangle() = default;
  Rectangle(double llx, double lly, double urx, double ury);

  double llx() const { return _llx; }
  double lly() const { return _lly; }
  double urx() const { return _urx; }
  double ury() const { return _ury; }

  double width() const;
  double height() const;
  double area() const;

 private:
  double _llx{};
  double _lly{};
  double _urx{};
  double _ury{};
};

/// Axis-aligned intersection; empty if disjoint or degenerate.
optional<Rectangle> intersect_rectangles(const Rectangle& a, const Rectangle& b);

/// One RTLMP floorplan region from root.fp.txt (label plus bounds in µm).
class FpBox : public Rectangle {
 public:
  FpBox() = default;
  FpBox(string region_name, const Rectangle& bounds);

  const string& regionName() const { return _regionName; }

 private:
  string _regionName;
};

class Layout {
 public:
  const Rectangle& die() const { return _die; }
  const Rectangle& core() const { return _core; }

  double haloHorizontalUm() const { return _haloHorizontalUm; }
  double haloVerticalUm() const { return _haloVerticalUm; }

  const string& odbPath() const { return _odbPath; }
  const string& defPath() const { return _defPath; }

 private:
  friend class detail::ChipLoader;

  Rectangle _die;
  Rectangle _core;
  double _haloHorizontalUm{};
  double _haloVerticalUm{};
  string _odbPath;
  string _defPath;
};

class Kit {
 public:
  const string& techLefPath() const { return _techLefPath; }
  const string& stdCellLefPath() const { return _stdCellLefPath; }
  const vector<string>& extraLefPaths() const { return _extraLefPaths; }
  const vector<string>& libertyPaths() const { return _libertyPaths; }

 private:
  friend class detail::ChipLoader;

  string _techLefPath;
  string _stdCellLefPath;
  vector<string> _extraLefPaths;
  vector<string> _libertyPaths;
};

struct LayerRc {
  string layer;
  double r{};
  double c{};
};

struct Stripe {
  string grid;
  string layer;
  double width{};
  double pitch{};
  double offset{};
  bool followpins{};
};

struct PgConn {
  string net;
  string inst_pattern;
  string pin_pattern;
  string rail;
};

struct VDomain {
  string name;
  string power_net;
  string gnd_net;
};

struct PdnGrid {
  string name;
  vector<string> voltage_domains;
  vector<string> pins;
  bool macro{};
  string orient;
  double halo[4]{};
  bool has_halo{};
  bool is_default{};
};

struct PdnConnect {
  string grid;
  string layer_lower;
  string layer_upper;
};

struct TrackRule {
  string layer;
  double x0{};
  double xp{};
  double y0{};
  double yp{};
};

class Tech {
 public:
  int processNm() const { return _processNm; }
  const string& siteName() const { return _siteName; }
  const string& routeMinLayer() const { return _routeMinLayer; }
  const string& routeMaxLayer() const { return _routeMaxLayer; }
  const string& routeClkMinLayer() const { return _routeClkMinLayer; }
  const string& signalWireRcLayer() const { return _signalWireRcLayer; }
  const string& clockWireRcLayer() const { return _clockWireRcLayer; }

  const vector<LayerRc>& layerResistanceCapacitance() const { return _layerRc; }
  const vector<TrackRule>& trackRules() const { return _trackRules; }

  bool globalConnectCalledInScript() const { return _globalConnectCalled; }
  const vector<PgConn>& globalConnections() const { return _globalConnections; }
  const vector<VDomain>& voltageDomains() const { return _voltageDomains; }
  const vector<PdnGrid>& pdnGrids() const { return _pdnGrids; }
  const vector<PdnConnect>& pdnConnects() const { return _pdnConnects; }
  const vector<Stripe>& stripes() const { return _stripes; }

 private:
  friend class detail::ChipLoader;

  int _processNm{};
  string _siteName;
  string _routeMinLayer;
  string _routeMaxLayer;
  string _routeClkMinLayer;
  string _signalWireRcLayer;
  string _clockWireRcLayer;
  vector<LayerRc> _layerRc;
  vector<TrackRule> _trackRules;

  bool _globalConnectCalled{};
  vector<PgConn> _globalConnections;
  vector<VDomain> _voltageDomains;
  vector<PdnGrid> _pdnGrids;
  vector<PdnConnect> _pdnConnects;
  vector<Stripe> _stripes;
};

struct InstPower {
  double internal_W{};
  double switching_W{};
  double leakage_W{};
  double total_W{};
  double current_A{};
  bool has_current{};
};

struct MacroPgPin {
  string name;
  double x_um{};
  double y_um{};
};

class Instance {
 public:
  explicit Instance(string instance_name);

  const string& instanceName() const { return _instanceName; }

  bool hasCluster() const { return _hasCluster; }
  int clusterId() const { return _clusterId; }
  const string& clusterName() const { return _clusterName; }

  bool isMacro() const { return _isMacro; }

  bool hasArea() const { return _hasArea; }
  double areaUm2() const { return _areaUm2; }

  bool hasLocation() const { return _hasLocation; }
  double centerXUm() const { return _centerXUm; }
  double centerYUm() const { return _centerYUm; }

  const vector<MacroPgPin>& pgPins() const { return _pgPins; }
  bool hasPgPins() const { return _hasPgPins; }

  bool hasManualPower() const { return _hasManualPower; }
  double manualPowerW() const { return _manualPowerW; }

  bool hasStaPower() const { return _hasStaPower; }
  const InstPower& staPower() const { return _staPower; }

 private:
  friend class detail::ChipLoader;

  string _instanceName;

  bool _hasCluster{};
  int _clusterId{};
  string _clusterName;

  bool _isMacro{};
  bool _hasArea{};
  double _areaUm2{};
  bool _hasLocation{};
  double _centerXUm{};
  double _centerYUm{};

  vector<MacroPgPin> _pgPins;
  bool _hasPgPins{};

  bool _hasManualPower{};
  double _manualPowerW{};

  bool _hasStaPower{};
  InstPower _staPower;
};

/// Top-level design bundle produced from manifest.json (paths, technology, instances).
class Chip {
 public:
  Chip() = default;

  const string& designName() const { return _designName; }

  const Layout& layout() const { return _layout; }
  const Kit& kit() const { return _kit; }
  const Tech& tech() const { return _tech; }

  const unordered_map<string, Instance>& instances() const { return _instances; }

  const vector<FpBox>& floorplanRegions() const { return _floorplanRegions; }

  /// OpenROAD-flow-scripts root used to resolve manifest-relative paths (same as ChipLoader).
  const fs::path& repoRoot() const { return _repoRoot; }

  /// Optional path (manifest-relative to repo root): OpenROAD PSM `-vsrc` file (`x,y,size,voltage` per line,
  /// µm — see `tools/OpenROAD/src/psm/src/ir_solver.cpp` `generateSourceNodesFromSourceFile`). If empty,
  /// sources follow `generateSourceNodesGenericBumps`-style pattern on `layout.core()` (mesh domain).
  /// When `psm_vsrc_boxes_<NET>.tsv` exists next to this file (from `export_psm_vsrc.tcl`), phys_load prefers
  /// true BPin rectangles and clips them to `layout.core()` (same domain as the uniform mesh).
  const string& psmVsrcFile() const { return _psmVsrcFile; }

  /// Optional override: true ODB pin boxes in µm (`llx lly urx ury voltage` plus optional `layer`,
  /// `bterm` from export_psm_vsrc.tcl), tab or comma.
  /// If empty, phys_load looks for `psm_vsrc_boxes_<NET>.tsv` beside `psm_vsrc_<NET>.loc`.
  const string& psmVsrcBoxesFile() const { return _psmVsrcBoxesFile; }

  /// Optional: real PDN via (x,y) from ODB export (`pdn_vias_<NET>.tsv`). When empty, the IR
  /// solver places synthetic vias at every load-mesh node for each `add_pdn_connect` pair
  /// (see `PHYS_PDN_PARSE_SOURCE` / `pdn_model.cpp`).
  const string& pdnViasFile() const { return _pdnViasFile; }

  /// Optional ODB-derived PDN wire dump (`db_pdn_shapes.tsv`). Used only when
  /// `PHYS_PDN_PARSE_SOURCE=odb`; default Tcl path uses `add_pdn_stripe` from `pdn_tcl` instead.
  const string& pdnShapesFile() const { return _pdnShapesFile; }

  /// If true, each exported source rectangle fixes **one** mesh node (nearest grid point to the
  /// rectangle center among nodes inside the rect, else global nearest). If false (default), every
  /// mesh node inside the rectangle is fixed (ideal Vdd patch)—can cover bogus interior area when
  /// `.loc` squares are huge vs. the real pad.
  bool irVsrcCenterNodeOnly() const { return _irVsrcCenterNodeOnly; }

  /// Returns an existing instance or creates a placeholder row keyed by name.
  Instance& instanceOrInsert(const string& name);

 private:
  friend class detail::ChipLoader;

  string _designName;
  fs::path _repoRoot;
  string _psmVsrcFile;
  string _psmVsrcBoxesFile;
  string _pdnViasFile;
  string _pdnShapesFile;
  bool _irVsrcCenterNodeOnly{};

  Layout _layout;
  Kit _kit;
  Tech _tech;

  unordered_map<string, Instance> _instances;
  vector<FpBox> _floorplanRegions;
};

Chip load_chip(const fs::path& manifest_path);

/// Primary supply voltage (V) from ORFS `config.mk` variable `PWR_NETS_VOLTAGES` (same source as
/// `flow/scripts/final_report.tcl` / `set_pdnsim_net_voltage`). Tries
/// `flow/designs/<platform>/<design>/config.mk` then `flow/platforms/<platform>/config.mk`;
/// `<platform>` is inferred from `kit.techLefPath()` (`flow/platforms/<platform>/...`).
/// Returns nullopt if unset, unreadable, or the assignment uses make `$(...)` substitution.
optional<double> flow_supply_voltage_V(const Chip& chip);

}  // namespace phys

#endif  // PHYS_CHIP_HPP
