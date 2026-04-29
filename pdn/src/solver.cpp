#include <pdn/solver.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdn {
namespace {

double dot(const std::vector<double>& a, const std::vector<double>& b)
{
  double sum = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    sum += a[i] * b[i];
  }
  return sum;
}

void multiply(const std::vector<double>& diagonal,
              const std::vector<std::vector<std::pair<int, double>>>& off_diagonal,
              const std::vector<double>& x,
              std::vector<double>* out)
{
  out->assign(diagonal.size(), 0.0);
  for (std::size_t i = 0; i < diagonal.size(); ++i) {
    double value = diagonal[i] * x[i];
    for (const auto& edge : off_diagonal[i]) {
      value += edge.second * x[static_cast<std::size_t>(edge.first)];
    }
    (*out)[i] = value;
  }
}

}  // namespace

SolveResult DcSolver::solve(const ResistiveNetwork& network, const SolveOptions& options)
{
  if (network.nodes.empty()) {
    throw std::runtime_error("Cannot solve an empty network.");
  }

  std::vector<int> unknown_map(network.nodes.size(), -1);
  std::vector<NodeId> unknown_nodes;
  unknown_nodes.reserve(network.nodes.size());
  for (const Node& node : network.nodes) {
    if (!node.fixed_voltage) {
      unknown_map[static_cast<std::size_t>(node.id)] = static_cast<int>(unknown_nodes.size());
      unknown_nodes.push_back(node.id);
    }
  }

  const int unknown_count = static_cast<int>(unknown_nodes.size());
  if (unknown_count == 0) {
    SolveResult result;
    result.node_voltages_v.resize(network.nodes.size(), 0.0);
    result.min_voltage_v = network.nodes.front().fixed_voltage_v;
    result.max_voltage_v = network.nodes.front().fixed_voltage_v;
    for (const Node& node : network.nodes) {
      result.node_voltages_v[static_cast<std::size_t>(node.id)] = node.fixed_voltage_v;
      result.min_voltage_v = std::min(result.min_voltage_v, node.fixed_voltage_v);
      result.max_voltage_v = std::max(result.max_voltage_v, node.fixed_voltage_v);
    }
    return result;
  }

  std::vector<double> diagonal(static_cast<std::size_t>(unknown_count), 0.0);
  std::vector<double> rhs(static_cast<std::size_t>(unknown_count), 0.0);
  std::vector<std::unordered_map<int, double>> off_map(static_cast<std::size_t>(unknown_count));

  for (const Node& node : network.nodes) {
    if (node.fixed_voltage) {
      continue;
    }
    const int row = unknown_map.at(static_cast<std::size_t>(node.id));
    rhs[static_cast<std::size_t>(row)] = node.injected_current_a;
  }

  for (const Edge& edge : network.edges) {
    const double g = edge.conductanceSiemens();
    if (g <= 0.0) {
      continue;
    }

    const Node& from = network.nodes.at(static_cast<std::size_t>(edge.from));
    const Node& to = network.nodes.at(static_cast<std::size_t>(edge.to));
    const bool from_unknown = !from.fixed_voltage;
    const bool to_unknown = !to.fixed_voltage;

    if (from_unknown) {
      const int row = unknown_map.at(static_cast<std::size_t>(from.id));
      diagonal[static_cast<std::size_t>(row)] += g;
      if (to_unknown) {
        const int col = unknown_map.at(static_cast<std::size_t>(to.id));
        off_map[static_cast<std::size_t>(row)][col] -= g;
      } else {
        rhs[static_cast<std::size_t>(row)] += g * to.fixed_voltage_v;
      }
    }

    if (to_unknown) {
      const int row = unknown_map.at(static_cast<std::size_t>(to.id));
      diagonal[static_cast<std::size_t>(row)] += g;
      if (from_unknown) {
        const int col = unknown_map.at(static_cast<std::size_t>(from.id));
        off_map[static_cast<std::size_t>(row)][col] -= g;
      } else {
        rhs[static_cast<std::size_t>(row)] += g * from.fixed_voltage_v;
      }
    }
  }

  std::vector<std::vector<std::pair<int, double>>> off_diagonal(static_cast<std::size_t>(unknown_count));
  for (int i = 0; i < unknown_count; ++i) {
    if (diagonal[static_cast<std::size_t>(i)] <= 0.0) {
      throw std::runtime_error("Solver encountered an isolated unknown node.");
    }
    for (const auto& entry : off_map[static_cast<std::size_t>(i)]) {
      off_diagonal[static_cast<std::size_t>(i)].push_back(entry);
    }
  }

  std::vector<double> x(static_cast<std::size_t>(unknown_count), 0.0);
  for (int i = 0; i < unknown_count; ++i) {
    x[static_cast<std::size_t>(i)] = rhs[static_cast<std::size_t>(i)] / diagonal[static_cast<std::size_t>(i)];
  }

  std::vector<double> residual(static_cast<std::size_t>(unknown_count), 0.0);
  std::vector<double> z(static_cast<std::size_t>(unknown_count), 0.0);
  std::vector<double> direction(static_cast<std::size_t>(unknown_count), 0.0);
  std::vector<double> ap(static_cast<std::size_t>(unknown_count), 0.0);

  multiply(diagonal, off_diagonal, x, &ap);
  for (int i = 0; i < unknown_count; ++i) {
    residual[static_cast<std::size_t>(i)] = rhs[static_cast<std::size_t>(i)] - ap[static_cast<std::size_t>(i)];
    z[static_cast<std::size_t>(i)] = residual[static_cast<std::size_t>(i)] / diagonal[static_cast<std::size_t>(i)];
    direction[static_cast<std::size_t>(i)] = z[static_cast<std::size_t>(i)];
  }

  const double rhs_norm = std::sqrt(std::max(dot(rhs, rhs), 1e-30));
  double rz = dot(residual, z);
  int iterations = 0;
  for (; iterations < options.max_iterations; ++iterations) {
    multiply(diagonal, off_diagonal, direction, &ap);
    const double denom = dot(direction, ap);
    if (std::abs(denom) <= 1e-30) {
      break;
    }

    const double alpha = rz / denom;
    for (int i = 0; i < unknown_count; ++i) {
      x[static_cast<std::size_t>(i)] += alpha * direction[static_cast<std::size_t>(i)];
      residual[static_cast<std::size_t>(i)] -= alpha * ap[static_cast<std::size_t>(i)];
    }

    const double residual_norm = std::sqrt(std::max(dot(residual, residual), 0.0));
    if (residual_norm <= options.relative_tolerance * rhs_norm) {
      ++iterations;
      break;
    }

    for (int i = 0; i < unknown_count; ++i) {
      z[static_cast<std::size_t>(i)] = residual[static_cast<std::size_t>(i)] / diagonal[static_cast<std::size_t>(i)];
    }
    const double rz_next = dot(residual, z);
    const double beta = rz_next / rz;
    rz = rz_next;

    for (int i = 0; i < unknown_count; ++i) {
      direction[static_cast<std::size_t>(i)] = z[static_cast<std::size_t>(i)]
                                               + beta * direction[static_cast<std::size_t>(i)];
    }
  }

  SolveResult result;
  result.node_voltages_v.resize(network.nodes.size(), 0.0);
  result.iterations = iterations;
  result.residual_l2 = std::sqrt(std::max(dot(residual, residual), 0.0));
  result.min_voltage_v = std::numeric_limits<double>::infinity();
  result.max_voltage_v = -std::numeric_limits<double>::infinity();

  for (const Node& node : network.nodes) {
    double voltage = node.fixed_voltage_v;
    if (!node.fixed_voltage) {
      const int row = unknown_map.at(static_cast<std::size_t>(node.id));
      voltage = x[static_cast<std::size_t>(row)];
    }
    result.node_voltages_v[static_cast<std::size_t>(node.id)] = voltage;
    result.min_voltage_v = std::min(result.min_voltage_v, voltage);
    result.max_voltage_v = std::max(result.max_voltage_v, voltage);
  }

  return result;
}

}  // namespace pdn
