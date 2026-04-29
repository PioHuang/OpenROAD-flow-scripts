#ifndef PDN_NETWORK_HPP
#define PDN_NETWORK_HPP

#include <pdn/geometry.hpp>
#include <pdn/spec.hpp>

#include <string>
#include <vector>

namespace pdn {

using NodeId = int;

enum class EdgeKind {
  kWire,
  kVia,
};

struct Node {
  NodeId id {-1};
  int layer_index {-1};
  std::string layer_name;
  Point location_um;
  bool fixed_voltage {};
  double fixed_voltage_v {};
  double injected_current_a {};
};

struct Edge {
  NodeId from {-1};
  NodeId to {-1};
  EdgeKind kind {EdgeKind::kWire};
  double resistance_ohm {};

  double conductanceSiemens() const
  {
    return resistance_ohm > 0.0 ? 1.0 / resistance_ohm : 0.0;
  }
};

struct LayerLattice {
  int layer_index {-1};
  std::string layer_name;
  Orientation orientation {Orientation::kHorizontal};
  int nx {};
  int ny {};
  double pitch_x_um {};
  double pitch_y_um {};
  double wire_step_um {};
  NodeId first_node {-1};
};

struct ResistiveNetwork {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<LayerLattice> lattices;
  int observation_layer_index {-1};

  const LayerLattice& observationLattice() const
  {
    return lattices.at(static_cast<std::size_t>(observation_layer_index));
  }
};

}  // namespace pdn

#endif  // PDN_NETWORK_HPP
