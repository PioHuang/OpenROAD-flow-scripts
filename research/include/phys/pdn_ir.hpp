#pragma once

#include <phys/chip.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phys {

/// One strap-intersection sample on the core grid (um, V).
/// Tile = floorplan region nearest this node (midlines between nodes); loads
/// attach here per paper. Ring nodes additionally anchor the voltage source.
struct MeshNode {
  int ix{};
  int iy{};
  double x_um{};
  double y_um{};
  double v_V{};
  double I_soft_A{};  // sum_k Imax(A_ov(node,k), k) over soft modules k
  double I_hard_A{};  // hard macro PG pins assigned by Manhattan nearest node
  bool is_ring{};     // true if this node sits on a PDN ring/BTERM (V src)
  double v_src_V{};   // voltage source value for ring nodes; 0 otherwise
  int layer_idx{-1};          // layer index in MultiLayerMesh::layers (-1 = legacy single-layer)
  std::string layer_name;     // e.g. "metal1", "metal4", "metal7"
};

/// Core-area mesh. In synth mode the pitch matches the chosen PDN strap pitch
/// from Tcl. In odb mode the pitch comes from the real stripe intersections.
struct UniformMesh {
  int nx{};
  int ny{};
  double pitch_x_um{};
  double pitch_y_um{};
  std::vector<MeshNode> nodes;
  /// Flat indices (iy*nx+ix) of ring nodes; populated by markRing / dump build.
  std::vector<size_t> ring_nodes;
};

/// Per-layer metadata for the multi-layer PDN mesh.
struct LayerInfo {
  std::string name;       // e.g. "metal1", "metal4", "metal7"
  int idx{};              // 0-based layer index (bottom to top, e.g. M1=0, M4=1, M7=2)
  bool is_horizontal{};   // true for horizontal straps (M1 followpins, M7 straps)
  double width_um{};      // strap/rail width from grid strategy
  double r_per_um{};      // from setRC.tcl
  double pitch_um{};      // from grid strategy
  bool is_followpin{};    // true for followpin layer (M1)
  std::vector<double> x_coords;  // sorted unique x-coordinates for nodes on this layer
  std::vector<double> y_coords;  // sorted unique y-coordinates for nodes on this layer
  int nx{};               // number of x-coordinates
  int ny{};               // number of y-coordinates
};

/// Multi-layer PDN mesh mirroring the real PDN topology (e.g. M1-M4-M7).
/// Nodes live on per-layer grids; inter-layer via connections couple adjacent
/// layers. An adjacency list stores all conductances (same-layer strap and
/// inter-layer via) so the CG solver is topology-agnostic.
struct MultiLayerMesh {
  std::vector<LayerInfo> layers;
  std::vector<MeshNode> nodes;

  /// First node index of each layer in the flat nodes vector.
  /// layer_node_offset[layers.size()] == nodes.size().
  std::vector<size_t> layer_node_offset;

  /// Adjacency list: adj[i] = {(neighbor_idx, conductance)} for node i.
  std::vector<std::vector<std::pair<size_t, double>>> adj;

  /// Flat indices of ring/BTERM nodes (voltage sources).
  std::vector<size_t> ring_nodes;

  /// Pin layer name from define_pdn_grid -pins (e.g. "metal7").
  std::string pin_layer;

  /// Index of the load-attachment layer for soft modules (M1, followpin layer).
  int soft_layer_idx{-1};
  /// Index of the load-attachment layer for hard macros (M4, lowest strap layer).
  int hard_layer_idx{-1};

  /// Total node count.
  size_t size() const { return nodes.size(); }
};

/// Horizontal/vertical strap effective sheet R and strap widths (um) for
/// lumped mesh branches.
struct StrapSheetModel {
  double r_sqh{};
  double r_sqv{};
  double w_hstrap_um{};
  double w_vstrap_um{};
};

/// IR calculation mode.
/// Max  -> paper two-step: (1) CG mesh solve Gx=i, (2) local max(Rh,Rv) drop.
/// Sum  -> legacy lumped: I*(Rh+Rv) Manhattan path to nearest ring (no solve).
enum class IrMode { Sum, Max };

/// VDD, STA current preference, knapsack fallback area, effective fill of
/// cluster box, and IR combine mode.
struct EstOpts {
  double vdd_V{0.8};
  bool prefer_sta_current{true};
  double assumed_cell_area_um2{1.0};
  double packing_utilization{1.0};
  IrMode ir_mode{IrMode::Sum};
  double via_r_ohm{0.0};  // inter-layer via resistance; 0 = ideal short
};

/// Fractional-knapsack profile for one soft module (0-1 knapsack with
/// divisible items).
struct SoftKnapsackProfile {
  std::vector<double> I_{};
  std::vector<double> A_{};
  std::vector<double> cumA_{};
  std::vector<double> cumI_{};

  /// Imax(W): max sum I s.t. sum area <= W (um^2), greedy by I/area.
  double imax(double cap_um2) const;
};

/// One RTLMP soft cluster: observed STA I, whole-box knapsack bound, mesh-
/// allocated Imax sum.
struct ModuleCurrent {
  int cluster_id{};
  std::string cluster_name;
  double box_area_um2{};
  size_t instance_count{};
  double observed_total_A{};
  double worst_case_A{};   // Imax(entire floorplan box * packing util) bound
  double i_mesh_sum_A{};   // sum_n Imax(A_ov(n,k), k) over mesh tiles
};

/// One hard-macro PG pin: instance supply current is split equally across
/// all pins.
struct HardMacroCurrent {
  std::string instance;
  std::string fp_region;
  std::string pg_pin_name;  ///< MTerm name when present in CSV
  double current_A{};       ///< Share of instance current at this pin (A)
  double pin_x_um{};
  double pin_y_um{};
};

/// Soft clusters + knapsack profiles (one pass over instances).
struct SoftIrData {
  std::vector<ModuleCurrent> modules;
  std::unordered_map<std::string, SoftKnapsackProfile> knapsack_by_name;
};

/// One segment from the real PDN dump (dump_pdn_mesh.tcl). Coordinates in um.
/// kind is one of "STRIPE", "RING", "FOLLOWPIN", "BTERM".
struct PdnSegment {
  std::string kind;
  std::string layer;
  double xlo{};
  double ylo{};
  double xhi{};
  double yhi{};
};

/// Parsed output of dump_pdn_mesh.tcl.
struct PdnDump {
  std::vector<PdnSegment> segs;
};

/// Parse the CSV produced by flow/scripts/dump_pdn_mesh.tcl.
/// Columns (tab-separated, one header line accepted):
///   kind  layer  xlo_um  ylo_um  xhi_um  yhi_um
PdnDump load_pdn_dump(const std::filesystem::path& csv);

/// Facade for PDN mesh IR: strap model, estimation options, mesh, loads, and
/// paper-style lumped IR drop. The Chip must outlive this object.
class IrModel {
 public:
  explicit IrModel(const Chip& chip);

  EstOpts& estOptions();
  const EstOpts& estOptions() const;

  void setStrapSheet(const StrapSheetModel& strap);
  const StrapSheetModel& strapSheet() const;

  /// Synth mesh: regular grid on chip.layout.core; node voltages init to
  /// estOptions().vdd_V. markRing() is called automatically (outermost ring).
  void buildUniformMesh(double pitch_x_um, double pitch_y_um);

  /// ODB mesh (legacy single-layer): build a non-uniform grid from real PDN
  /// stripe intersections. Ring nodes are covered by RING segments.
  void buildMeshFromPdnDump(const PdnDump& dump,
                            const std::unordered_map<std::string, double>&
                                rc_r_per_um,
                            double v_supply_V);

  /// ODB multi-layer mesh: build a 3D mesh (e.g. M1-M4-M7) from the PDN
  /// dump, respecting the layer hierarchy from grid_strategy.  Voltage
  /// sources are placed at BTERM locations on the pin layer (or RING
  /// segments, or outer-ring fallback). Soft loads attach to the bottom
  /// layer (M1); hard loads attach to the lowest strap layer (M4).
  void buildMultiLayerMesh(const PdnDump& dump,
                           const std::unordered_map<std::string, double>&
                               rc_r_per_um,
                           double v_supply_V);

  /// Mark the outermost ring of mesh nodes as voltage-source anchors. Called
  /// internally by buildUniformMesh; safe to call again.
  void markOuterRing(double v_supply_V);

  void loadHardMacroCurrents();
  void analyzeSoftModules();
  void assignMeshLoads();

  /// Voltage at a hard PG pin. When ir_mode==Max (paper), uses the CG-solved
  /// mesh voltage V_i at the nearest node minus a local drop to the pin.
  /// When ir_mode==Sum (legacy), lumped path to nearest ring node.
  double hardPinVoltage(double pin_x_um,
                        double pin_y_um,
                        double current_A) const;

  /// Pessimistic minimum voltage under a soft cluster box. When ir_mode==Max,
  /// uses CG-solved V_i minus local drop to the farthest overlap corner.
  /// When ir_mode==Sum, lumped path per tile to nearest ring node.
  double softClusterWorstVoltage(const FpBox& module_box,
                                 const SoftKnapsackProfile& kn) const;

  /// Same as above; looks up knapsack by cluster_name in softData().
  double softClusterWorstVoltage(const FpBox& module_box,
                                 const std::string& cluster_name) const;

  /// Solve Gx=i on the mesh using Jacobi-preconditioned CG. Ring nodes are
  /// fixed at v_src_V; internal nodes receive -(I_soft+I_hard) as injected
  /// current. Result is stored in mesh nodes' v_V.
  void solveMeshVoltages();

  /// Per-mesh-node voltage. When ir_mode==Max (paper two-step), returns the
  /// CG-solved v_V. When ir_mode==Sum (legacy lumped), returns
  /// v_src(r) - I*R(node -> r). Ring nodes always return v_src_V.
  double nodeVoltage(size_t node_idx) const;

  /// Populate each mesh node's v_V. When ir_mode==Max, calls
  /// solveMeshVoltages() (CG solve). When ir_mode==Sum, uses per-node lumped
  /// formula. Call after assignMeshLoads().
  void updateMeshVoltages();

  const Chip& chip() const { return *chip_; }

  /// Legacy single-layer mesh (synth / odb single-layer mode).
  const UniformMesh& mesh() const { return mesh_; }
  UniformMesh& mesh() { return mesh_; }

  /// Multi-layer mesh (odb multi-layer mode).
  const MultiLayerMesh& multiMesh() const { return ml_mesh_; }
  MultiLayerMesh& multiMesh() { return ml_mesh_; }

  bool isMultiLayer() const { return multi_layer_; }

  const std::vector<HardMacroCurrent>& hardMacros() const { return hard_; }
  const SoftIrData& softData() const { return soft_; }
  SoftIrData& softData() { return soft_; }

 private:
  const Chip* chip_{};
  StrapSheetModel strap_{};
  EstOpts est_{};
  UniformMesh mesh_{};
  MultiLayerMesh ml_mesh_{};
  bool multi_layer_{};
  std::vector<HardMacroCurrent> hard_{};
  SoftIrData soft_{};
};

}  // namespace phys
