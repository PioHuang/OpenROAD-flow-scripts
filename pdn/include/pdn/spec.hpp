#ifndef PDN_SPEC_HPP
#define PDN_SPEC_HPP

#include <pdn/geometry.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace pdn {

enum class Orientation {
  kHorizontal,
  kVertical,
};

struct MetalLayer {
  std::string name;
  Orientation orientation {Orientation::kHorizontal};
  int rank {};
  double pitch_um {};
  double width_um {};
  double sheet_resistance_ohm_per_square {};

  double segmentResistanceOhm(double length_um) const
  {
    if (width_um <= 0.0) {
      throw std::runtime_error("MetalLayer width must be positive.");
    }
    return sheet_resistance_ohm_per_square * length_um / width_um;
  }
};

struct ViaConnection {
  std::string lower_layer;
  std::string upper_layer;
  double resistance_ohm {};
};

struct VoltageSource {
  std::string layer;
  Rect region_um;
  double voltage_v {};
};

struct PointLoad {
  std::string name;
  std::string layer;
  Point location_um;
  double current_a {};
};

struct DistributedLoad {
  std::string name;
  std::string layer;
  Rect region_um;
  double total_current_a {};
};

struct PdnProblem {
  Rect core_um;
  std::vector<MetalLayer> layers;
  std::vector<ViaConnection> via_connections;
  std::vector<VoltageSource> voltage_sources;
  std::vector<PointLoad> point_loads;
  std::vector<DistributedLoad> distributed_loads;
  std::string observation_layer;

  // If non-zero, this overrides the observation layer lattice pitch. This is
  // the knob intended for floorplan-stage soft-load abstraction.
  double observation_pitch_um {};
};

}  // namespace pdn

#endif  // PDN_SPEC_HPP
