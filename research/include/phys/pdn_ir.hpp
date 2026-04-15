#pragma once

#include <phys/chip.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace phys {

/// One strap-intersection sample on the uniform core grid (µm, V).
/// Tile = floorplan region nearest this node (midlines between nodes); loads attach here per paper.
struct MeshNode {
  int ix{};
  int iy{};
  double x_um{};
  double y_um{};
  double v_V{};
  double I_soft_A{};  // sum_k Imax(A_ov(node,k), k) over soft modules k
  double I_hard_A{};  // hard macro PG pins assigned by Manhattan nearest node
};

/// Regular grid over layout.core; pitch usually matches PDN strap pitch from Tcl.
struct UniformMesh {
  int nx{};
  int ny{};
  double pitch_x_um{};
  double pitch_y_um{};
  std::vector<MeshNode> nodes;
};

/// Horizontal/vertical strap effective sheet R and strap widths (µm) for lumped mesh branches.
struct StrapSheetModel {
  double r_sqh{};
  double r_sqv{};
  double w_hstrap_um{};
  double w_vstrap_um{};
};

/// VDD, STA current preference, knapsack fallback area, and effective fill of cluster box.
struct EstOpts {
  double vdd_V{0.8};
  bool prefer_sta_current{true};
  double assumed_cell_area_um2{1.0};
  double packing_utilization{1.0};
};

/// Fractional-knapsack profile for one soft module (0–1 knapsack approx: divisible items).
struct SoftKnapsackProfile {
  std::vector<double> I_{};
  std::vector<double> A_{};
  std::vector<double> cumA_{};
  std::vector<double> cumI_{};

  /// Imax(W): max Σ I with Σ area ≤ W (µm²), greedy by I/area.
  double imax(double cap_um2) const;
};

/// One RTLMP soft cluster: observed STA I, whole-box knapsack bound, mesh-allocated Imax sum.
struct ModuleCurrent {
  int cluster_id{};
  std::string cluster_name;
  double box_area_um2{};
  size_t instance_count{};
  double observed_total_A{};
  double worst_case_A{};           // Imax(entire floorplan box × packing util) — global bound
  double i_mesh_sum_A{};           // Σ_n Imax(A_ov(n,k), k) over mesh tiles (regional model)
};

/// One hard-macro PG pin: instance supply current is split equally across all pins (see hard_currents).
struct HardMacroCurrent {
  std::string instance;
  std::string fp_region;
  std::string pg_pin_name;  ///< MTerm name when present in TSV
  double current_A{};       ///< Share of instance current at this pin (A)
  double pin_x_um{};
  double pin_y_um{};
};

/// Soft clusters + knapsack profiles (one pass over instances).
struct SoftIrData {
  std::vector<ModuleCurrent> modules;
  std::unordered_map<std::string, SoftKnapsackProfile> knapsack_by_name;
};

/// Facade for PDN mesh IR: strap model, estimation options, mesh, loads, and DC solve.
/// The Chip must outlive this object.
class IrModel {
public:
  explicit IrModel(const Chip& chip);

  EstOpts& estOptions();
  const EstOpts& estOptions() const;

  void setStrapSheet(const StrapSheetModel& strap);
  const StrapSheetModel& strapSheet() const;

  /// Regular grid on chip.layout.core; node voltages initialized to estOptions().vdd_V.
  void buildUniformMesh(double pitch_x_um, double pitch_y_um);

  void loadHardMacroCurrents();
  void analyzeSoftModules();
  void assignMeshLoads();
  void solvePgMeshDc(double v_supply_V, int max_cg_iter = 0, double cg_tol_rel = 1e-8);

  /// After solve: estimated voltage at a hard PG pin (Manhattan nearest node + lumped strap drop).
  double hardPinVoltage(double pin_x_um, double pin_y_um, double current_A) const;

  /// After solve: pessimistic minimum voltage under a soft cluster box (tile-wise knapsack + ir_drop).
  double softClusterWorstVoltage(const FpBox& module_box, const SoftKnapsackProfile& kn) const;

  /// Same as overload above; looks up knapsack by cluster_name in softData().
  double softClusterWorstVoltage(const FpBox& module_box, const std::string& cluster_name) const;

  const Chip& chip() const { return *chip_; }
  const UniformMesh& mesh() const { return mesh_; }
  UniformMesh& mesh() { return mesh_; }
  const std::vector<HardMacroCurrent>& hardMacros() const { return hard_; }
  const SoftIrData& softData() const { return soft_; }
  SoftIrData& softData() { return soft_; }

private:
  const Chip* chip_{};
  StrapSheetModel strap_{};
  EstOpts est_{};
  UniformMesh mesh_{};
  std::vector<HardMacroCurrent> hard_{};
  SoftIrData soft_{};
};

}  // namespace phys
