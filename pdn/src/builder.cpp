#include <pdn/builder.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace pdn {
namespace {

int countSteps(double lo_um, double hi_um, double pitch_um)
{
  if (pitch_um <= 0.0) {
    throw std::runtime_error("Grid pitch must be positive.");
  }
  return static_cast<int>(std::floor((hi_um - lo_um) / pitch_um)) + 1;
}

int indexFor(int ix, int iy, int nx)
{
  return iy * nx + ix;
}

const MetalLayer& requireLayer(const PdnProblem& problem, const std::string& layer_name, int* layer_index)
{
  for (std::size_t i = 0; i < problem.layers.size(); ++i) {
    if (problem.layers[i].name == layer_name) {
      *layer_index = static_cast<int>(i);
      return problem.layers[i];
    }
  }
  throw std::runtime_error("Unknown layer: " + layer_name);
}

NodeId nearestNodeOnLayer(const ResistiveNetwork& network, int layer_index, const Point& target)
{
  const LayerLattice& lattice = network.lattices.at(static_cast<std::size_t>(layer_index));
  if (lattice.first_node < 0) {
    throw std::runtime_error("Layer lattice has no nodes.");
  }

  NodeId best = lattice.first_node;
  double best_distance = manhattanDistanceUm(network.nodes.at(static_cast<std::size_t>(best)).location_um,
                                             target);
  const int count = lattice.nx * lattice.ny;
  for (int i = 1; i < count; ++i) {
    const NodeId candidate = lattice.first_node + i;
    const double distance
        = manhattanDistanceUm(network.nodes.at(static_cast<std::size_t>(candidate)).location_um, target);
    if (distance < best_distance) {
      best_distance = distance;
      best = candidate;
    }
  }
  return best;
}

std::vector<NodeId> nodesInsideRegion(const ResistiveNetwork& network, int layer_index, const Rect& region_um)
{
  std::vector<NodeId> nodes;
  const LayerLattice& lattice = network.lattices.at(static_cast<std::size_t>(layer_index));
  const int count = lattice.nx * lattice.ny;
  for (int i = 0; i < count; ++i) {
    const NodeId node_id = lattice.first_node + i;
    if (region_um.contains(network.nodes.at(static_cast<std::size_t>(node_id)).location_um)) {
      nodes.push_back(node_id);
    }
  }
  return nodes;
}

double layerPitchForAnalysis(const PdnProblem& problem, const MetalLayer& layer)
{
  if (layer.name == problem.observation_layer && problem.observation_pitch_um > 0.0) {
    return problem.observation_pitch_um;
  }
  return layer.pitch_um;
}

bool useSheetAbstraction(const PdnProblem& problem, const MetalLayer& layer)
{
  return layer.name == problem.observation_layer && problem.observation_pitch_um > 0.0;
}

}  // namespace

ResistiveNetwork PdnBuilder::build(const PdnProblem& problem)
{
  if (problem.layers.empty()) {
    throw std::runtime_error("PdnProblem must contain at least one layer.");
  }
  if (problem.core_um.widthUm() <= 0.0 || problem.core_um.heightUm() <= 0.0) {
    throw std::runtime_error("Core rectangle must have positive area.");
  }
  if (problem.observation_layer.empty()) {
    throw std::runtime_error("Observation layer must be specified.");
  }

  ResistiveNetwork network;
  network.observation_layer_index = -1;

  std::unordered_map<std::string, int> layer_to_index;
  for (std::size_t i = 0; i < problem.layers.size(); ++i) {
    const MetalLayer& layer = problem.layers[i];
    if (layer.pitch_um <= 0.0 || layer.width_um <= 0.0 || layer.sheet_resistance_ohm_per_square <= 0.0) {
      throw std::runtime_error("Layer " + layer.name + " has invalid geometry or resistance.");
    }
    layer_to_index.emplace(layer.name, static_cast<int>(i));
    if (layer.name == problem.observation_layer) {
      network.observation_layer_index = static_cast<int>(i);
    }
  }
  if (network.observation_layer_index < 0) {
    throw std::runtime_error("Observation layer not present in layer list.");
  }

  for (std::size_t layer_index = 0; layer_index < problem.layers.size(); ++layer_index) {
    const MetalLayer& layer = problem.layers[layer_index];
    const double pitch_um = layerPitchForAnalysis(problem, layer);
    const bool sheet_layer = useSheetAbstraction(problem, layer);
    const int nx = countSteps(problem.core_um.llx_um, problem.core_um.urx_um, pitch_um);
    const int ny = countSteps(problem.core_um.lly_um, problem.core_um.ury_um, pitch_um);

    LayerLattice lattice;
    lattice.layer_index = static_cast<int>(layer_index);
    lattice.layer_name = layer.name;
    lattice.orientation = layer.orientation;
    lattice.nx = nx;
    lattice.ny = ny;
    lattice.pitch_x_um = pitch_um;
    lattice.pitch_y_um = pitch_um;
    lattice.wire_step_um = pitch_um;
    lattice.first_node = static_cast<NodeId>(network.nodes.size());

    for (int iy = 0; iy < ny; ++iy) {
      for (int ix = 0; ix < nx; ++ix) {
        Node node;
        node.id = static_cast<NodeId>(network.nodes.size());
        node.layer_index = static_cast<int>(layer_index);
        node.layer_name = layer.name;
        node.location_um = {
            problem.core_um.llx_um + static_cast<double>(ix) * pitch_um,
            problem.core_um.lly_um + static_cast<double>(iy) * pitch_um,
        };
        network.nodes.push_back(node);
      }
    }

    const double segment_resistance_ohm
        = sheet_layer ? layer.sheet_resistance_ohm_per_square : layer.segmentResistanceOhm(pitch_um);
    for (int iy = 0; iy < ny; ++iy) {
      for (int ix = 0; ix < nx; ++ix) {
        const NodeId node_id = lattice.first_node + indexFor(ix, iy, nx);
        if (sheet_layer || layer.orientation == Orientation::kHorizontal) {
          if (ix + 1 < nx) {
            network.edges.push_back({node_id,
                                     lattice.first_node + indexFor(ix + 1, iy, nx),
                                     EdgeKind::kWire,
                                     segment_resistance_ohm});
          }
        }
        if (sheet_layer || layer.orientation == Orientation::kVertical) {
          if (iy + 1 < ny) {
            network.edges.push_back({node_id,
                                     lattice.first_node + indexFor(ix, iy + 1, nx),
                                     EdgeKind::kWire,
                                     segment_resistance_ohm});
          }
        }
      }
    }

    network.lattices.push_back(lattice);
  }

  for (const ViaConnection& via : problem.via_connections) {
    auto lower_it = layer_to_index.find(via.lower_layer);
    auto upper_it = layer_to_index.find(via.upper_layer);
    if (lower_it == layer_to_index.end() || upper_it == layer_to_index.end()) {
      throw std::runtime_error("Via references unknown layer.");
    }
    if (via.resistance_ohm <= 0.0) {
      throw std::runtime_error("Via resistance must be positive.");
    }

    const LayerLattice& lower = network.lattices.at(static_cast<std::size_t>(lower_it->second));
    const LayerLattice& upper = network.lattices.at(static_cast<std::size_t>(upper_it->second));
    const double via_pitch_um = std::max(lower.pitch_x_um, upper.pitch_x_um);
    const int vx = countSteps(problem.core_um.llx_um, problem.core_um.urx_um, via_pitch_um);
    const int vy = countSteps(problem.core_um.lly_um, problem.core_um.ury_um, via_pitch_um);

    for (int iy = 0; iy < vy; ++iy) {
      for (int ix = 0; ix < vx; ++ix) {
        Point site {
            problem.core_um.llx_um + static_cast<double>(ix) * via_pitch_um,
            problem.core_um.lly_um + static_cast<double>(iy) * via_pitch_um,
        };
        const NodeId lower_node = nearestNodeOnLayer(network, lower.layer_index, site);
        const NodeId upper_node = nearestNodeOnLayer(network, upper.layer_index, site);
        network.edges.push_back({lower_node, upper_node, EdgeKind::kVia, via.resistance_ohm});
      }
    }
  }

  for (const VoltageSource& source : problem.voltage_sources) {
    int layer_index = -1;
    requireLayer(problem, source.layer, &layer_index);
    std::vector<NodeId> hit_nodes = nodesInsideRegion(network, layer_index, source.region_um);
    if (hit_nodes.empty()) {
      hit_nodes.push_back(nearestNodeOnLayer(network, layer_index, source.region_um.center()));
    }
    for (NodeId node_id : hit_nodes) {
      Node& node = network.nodes.at(static_cast<std::size_t>(node_id));
      node.fixed_voltage = true;
      node.fixed_voltage_v = source.voltage_v;
    }
  }

  for (const PointLoad& load : problem.point_loads) {
    int layer_index = -1;
    requireLayer(problem, load.layer, &layer_index);
    const NodeId node_id = nearestNodeOnLayer(network, layer_index, load.location_um);
    network.nodes.at(static_cast<std::size_t>(node_id)).injected_current_a -= load.current_a;
  }

  for (const DistributedLoad& load : problem.distributed_loads) {
    int layer_index = -1;
    requireLayer(problem, load.layer, &layer_index);
    std::vector<NodeId> targets = nodesInsideRegion(network, layer_index, load.region_um);
    if (targets.empty()) {
      targets.push_back(nearestNodeOnLayer(network, layer_index, load.region_um.center()));
    }
    const double current_per_node = load.total_current_a / static_cast<double>(targets.size());
    for (NodeId node_id : targets) {
      network.nodes.at(static_cast<std::size_t>(node_id)).injected_current_a -= current_per_node;
    }
  }

  return network;
}

}  // namespace pdn
