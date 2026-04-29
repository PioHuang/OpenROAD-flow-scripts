#ifndef PHYS_PDN_IR_HPP
#define PHYS_PDN_IR_HPP

#include <phys/chip.hpp>
#include <phys/pdn_model.hpp>

#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

using namespace std;

namespace phys {

/// One strap-intersection sample on the uniform core grid (µm, V).
class MeshNode {
 public:
  MeshNode() = default;

  int gridIndexX() const { return _ix; }
  int gridIndexY() const { return _iy; }
  double xUm() const { return _xUm; }
  double yUm() const { return _yUm; }
  double voltageV() const { return _voltageV; }
  double softCurrentA() const { return _softCurrentA; }
  double hardCurrentA() const { return _hardCurrentA; }

  void setGridIndices(int ix, int iy) {
    _ix = ix;
    _iy = iy;
  }
  void setPositionUm(double x, double y) {
    _xUm = x;
    _yUm = y;
  }
  void setVoltageV(double v) { _voltageV = v; }
  void addSoftCurrentA(double i) { _softCurrentA += i; }
  void addHardCurrentA(double i) { _hardCurrentA += i; }
  void resetLoads() {
    _softCurrentA = 0.0;
    _hardCurrentA = 0.0;
  }

 private:
  int _ix{};
  int _iy{};
  double _xUm{};
  double _yUm{};
  double _voltageV{};
  double _softCurrentA{};
  double _hardCurrentA{};
};

/// Regular grid over the core rectangle; pitch usually matches PDN strap pitch from Tcl.
class UniformMesh {
 public:
  int countX() const { return _nx; }
  int countY() const { return _ny; }
  double pitchXUm() const { return _pitchXUm; }
  double pitchYUm() const { return _pitchYUm; }

  const vector<MeshNode>& nodes() const { return _nodes; }
  vector<MeshNode>& nodes() { return _nodes; }

  void setDimensions(int nx, int ny, double pitch_x_um, double pitch_y_um) {
    _nx = nx;
    _ny = ny;
    _pitchXUm = pitch_x_um;
    _pitchYUm = pitch_y_um;
  }

 private:
  int _nx{};
  int _ny{};
  double _pitchXUm{};
  double _pitchYUm{};
  vector<MeshNode> _nodes;
};

/// Horizontal / vertical strap effective sheet R and strap widths (µm) for lumped mesh branches.
class StrapSheetModel {
 public:
  StrapSheetModel() = default;
  StrapSheetModel(double sheet_r_horizontal,
                  double sheet_r_vertical,
                  double horizontal_strap_width_um,
                  double vertical_strap_width_um);

  double sheetResistanceHorizontal() const { return _sheetResistanceHorizontal; }
  double sheetResistanceVertical() const { return _sheetResistanceVertical; }
  double horizontalStrapWidthUm() const { return _horizontalStrapWidthUm; }
  double verticalStrapWidthUm() const { return _verticalStrapWidthUm; }

  void setSheetResistanceHorizontal(double r) { _sheetResistanceHorizontal = r; }
  void setSheetResistanceVertical(double r) { _sheetResistanceVertical = r; }
  void setHorizontalStrapWidthUm(double w) { _horizontalStrapWidthUm = w; }
  void setVerticalStrapWidthUm(double w) { _verticalStrapWidthUm = w; }

 private:
  double _sheetResistanceHorizontal{};
  double _sheetResistanceVertical{};
  double _horizontalStrapWidthUm{};
  double _verticalStrapWidthUm{};
};

/// VDD, STA current preference, knapsack fallback area, and effective fill of cluster box.
class EstimationOptions {
 public:
  double supplyVoltageV() const { return _supplyVoltageV; }
  bool preferStaCurrent() const { return _preferStaCurrent; }
  double assumedCellAreaUm2() const { return _assumedCellAreaUm2; }
  double packingUtilization() const { return _packingUtilization; }

  void setSupplyVoltageV(double v) { _supplyVoltageV = v; }
  void setPreferStaCurrent(bool v) { _preferStaCurrent = v; }
  void setAssumedCellAreaUm2(double a) { _assumedCellAreaUm2 = a; }
  void setPackingUtilization(double u) { _packingUtilization = u; }

 private:
  double _supplyVoltageV{0.8};
  bool _preferStaCurrent{true};
  double _assumedCellAreaUm2{1.0};
  double _packingUtilization{1.0};
};

/// Fractional-knapsack profile for one soft module (0–1 knapsack approx: divisible items).
class SoftKnapsackProfile {
 public:
  /// Imax(W): max Σ I with Σ area ≤ W (µm²), greedy by I/area.
  double maxCurrentForAreaBudget(double cap_um2) const;

  /// Rebuilds internal prefix sums from (current, area) pairs; sorts by current/area descending.
  void rebuildFromItemPairs(vector<pair<double, double>> items);

 private:
  vector<double> _currentPerInstance{};
  vector<double> _areaPerInstance{};
  vector<double> _prefixArea{};
  vector<double> _prefixCurrent{};
};

/// One RTLMP soft cluster: observed STA I, whole-box knapsack bound, mesh-allocated Imax sum.
struct SoftModuleCurrent {
  int cluster_id{};
  string cluster_name;
  double box_area_um2{};
  size_t instance_count{};
  double observed_total_A{};
  double worst_case_A{};
  double i_mesh_sum_A{};
};

/// One hard-macro PG pin: instance supply current is split equally across all pins.
struct HardMacroPinCurrent {
  string instance;
  string fp_region;
  string pg_pin_name;
  double current_A{};
  double pin_x_um{};
  double pin_y_um{};
};

/// One hard-pin to graph-node attachment record for auditability.
struct HardPinGraphAttach {
  string instance;
  string fp_region;
  string pg_pin_name;
  double pin_x_um{};
  double pin_y_um{};
  double current_A{};
  int node_id{-1};
  string node_layer;
  double node_x_um{};
  double node_y_um{};
  double manhattan_um{};
};

struct MultiLayerSolveStats {
  struct LayerInfo {
    string name;
    bool horizontal{};
    double g_per_segment{};
    int rank{};
    double avg_v{};
    int unknown_nodes{};
    /// Nodes on this layer's own core grid (nx*ny); differs across layers when strap pitches differ.
    int node_count{};
    double pitch_um{};
  };
  struct ViaInfo {
    int from_layer{};
    int to_layer{};
    double g_via{};
  };
  vector<LayerInfo> layers;
  vector<ViaInfo> vias;
  int source_layer{};
  int load_layer{};
  int total_nodes{};
  int fixed_nodes{};
  int unknown_nodes{};
  int isolated_nodes{};
  int isolated_loaded_nodes{};
  int isolated_fixed_nodes{};
};

struct PdnGraphNode {
  int id{};
  int layer_idx{};
  string layer_name;
  double x_um{};
  double y_um{};
  double v_v{};
  bool fixed{};
  double i_load_a{};
};

struct PdnGraphEdge {
  int u{};
  int v{};
  string kind;  // "wire" or "via"
  double g{};
};

/// Soft clusters plus knapsack profiles (one pass over instances).
class SoftModuleIrData {
 public:
  const vector<SoftModuleCurrent>& modules() const { return _modules; }
  vector<SoftModuleCurrent>& modules() { return _modules; }

  const unordered_map<string, SoftKnapsackProfile>& knapsackByClusterName() const {
    return _knapsackByClusterName;
  }
  unordered_map<string, SoftKnapsackProfile>& knapsackByClusterName() {
    return _knapsackByClusterName;
  }

 private:
  vector<SoftModuleCurrent> _modules;
  unordered_map<string, SoftKnapsackProfile> _knapsackByClusterName;
};

/// VDD source rectangles (µm) clipped to `layout.core()`: manifest `psm_vsrc_boxes_file`, else
/// `psm_vsrc_file`. Throws if missing, unreadable, or all geometry lies outside core.
vector<Rectangle> psm_supply_source_rects_um(const Chip& chip);

/// PDN mesh IR: strap model, estimation options, mesh, loads, and DC solve. The Chip must outlive this object.
class IrModel {
 public:
  explicit IrModel(const Chip& chip);

  EstimationOptions& estimationOptions() { return _estimationOptions; }
  const EstimationOptions& estimationOptions() const { return _estimationOptions; }

  void setStrapSheetModel(const StrapSheetModel& strap);
  const StrapSheetModel& strapSheetModel() const { return _strapModel; }
  void setPdnModel(const PdnModel& pdn) { _pdnModel = pdn; }
  const PdnModel& pdnModel() const { return _pdnModel; }

  void buildUniformMesh(double pitch_x_um, double pitch_y_um);

  void loadHardMacroCurrents();
  void analyzeSoftModules();
  void assignMeshLoads();
  void solvePgMeshDc(double v_supply_V, int max_cg_iter = 0, double cg_tol_rel = 1e-8);

  double hardPinVoltage(double pin_x_um, double pin_y_um, double current_A) const;

  double softClusterWorstVoltage(const FpBox& module_box,
                                 const SoftKnapsackProfile& kn) const;

  double softClusterWorstVoltage(const FpBox& module_box, const string& cluster_name) const;

  const Chip& chip() const { return *_chip; }
  const UniformMesh& mesh() const { return _mesh; }
  UniformMesh& mesh() { return _mesh; }
  const MultiLayerSolveStats& multiLayerStats() const { return _mlStats; }
  const vector<PdnGraphNode>& pdnGraphNodes() const { return _pdnGraphNodes; }
  const vector<PdnGraphEdge>& pdnGraphEdges() const { return _pdnGraphEdges; }
  const vector<HardMacroPinCurrent>& hardMacros() const { return _hardMacros; }
  const vector<HardPinGraphAttach>& hardPinGraphAttach() const { return _hardPinGraphAttach; }
  const SoftModuleIrData& softModuleData() const { return _softModuleData; }
  SoftModuleIrData& softModuleData() { return _softModuleData; }

 private:
  const Chip* _chip{};
  StrapSheetModel _strapModel{};
  PdnModel _pdnModel{};
  EstimationOptions _estimationOptions{};
  UniformMesh _mesh{};
  MultiLayerSolveStats _mlStats{};
  vector<PdnGraphNode> _pdnGraphNodes{};
  vector<PdnGraphEdge> _pdnGraphEdges{};
  vector<HardMacroPinCurrent> _hardMacros{};
  vector<HardPinGraphAttach> _hardPinGraphAttach{};
  SoftModuleIrData _softModuleData{};
};

}  // namespace phys

#endif  // PHYS_PDN_IR_HPP
