#ifndef PDN_SOLVER_HPP
#define PDN_SOLVER_HPP

#include <pdn/network.hpp>

#include <vector>

namespace pdn {

struct SolveOptions {
  int max_iterations {20000};
  double relative_tolerance {1e-8};
};

struct SolveResult {
  std::vector<double> node_voltages_v;
  int iterations {};
  double residual_l2 {};
  double min_voltage_v {};
  double max_voltage_v {};
};

class DcSolver {
 public:
  static SolveResult solve(const ResistiveNetwork& network,
                           const SolveOptions& options = SolveOptions {});
};

}  // namespace pdn

#endif  // PDN_SOLVER_HPP
