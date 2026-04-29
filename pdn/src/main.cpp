#include <pdn/builder.hpp>
#include <pdn/io.hpp>
#include <pdn/network.hpp>
#include <pdn/solver.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string edgeKindName(pdn::EdgeKind kind)
{
  return kind == pdn::EdgeKind::kWire ? "wire" : "via";
}

void printUsage(const char* argv0)
{
  std::cerr << "usage: " << argv0 << " [problem.json] [--dump-dir DIR]\n";
  std::cerr << "default input: examples/example_problem.json\n";
}

void dumpTsv(const std::filesystem::path& dump_dir,
             const pdn::PdnProblem& problem,
             const pdn::ResistiveNetwork& network,
             const pdn::SolveResult& result)
{
  std::filesystem::create_directories(dump_dir);

  {
    std::ofstream out(dump_dir / "nodes.tsv");
    out << "id\tlayer_index\tlayer_name\tx_um\ty_um\tfixed_voltage\tfixed_voltage_v\tinjected_current_a\tvoltage_v\n";
    for (const pdn::Node& node : network.nodes) {
      const double voltage = result.node_voltages_v.at(static_cast<std::size_t>(node.id));
      out << node.id << '\t' << node.layer_index << '\t' << node.layer_name << '\t' << node.location_um.x_um
          << '\t' << node.location_um.y_um << '\t' << (node.fixed_voltage ? 1 : 0) << '\t'
          << node.fixed_voltage_v << '\t' << node.injected_current_a << '\t' << voltage << '\n';
    }
  }

  {
    std::ofstream out(dump_dir / "edges.tsv");
    out << "from\tto\tkind\tresistance_ohm\n";
    for (const pdn::Edge& edge : network.edges) {
      out << edge.from << '\t' << edge.to << '\t' << edgeKindName(edge.kind) << '\t' << edge.resistance_ohm
          << '\n';
    }
  }

  {
    std::ofstream out(dump_dir / "layers.tsv");
    out << "layer_index\tlayer_name\torientation\tnx\tny\tpitch_x_um\tpitch_y_um\twire_step_um\tfirst_node\n";
    for (const pdn::LayerLattice& layer : network.lattices) {
      out << layer.layer_index << '\t' << layer.layer_name << '\t'
          << (layer.orientation == pdn::Orientation::kHorizontal ? "horizontal" : "vertical") << '\t'
          << layer.nx << '\t' << layer.ny << '\t' << layer.pitch_x_um << '\t' << layer.pitch_y_um << '\t'
          << layer.wire_step_um << '\t' << layer.first_node << '\n';
    }
  }

  {
    std::ofstream out(dump_dir / "voltage_sources.tsv");
    out << "layer\tllx_um\tlly_um\turx_um\tury_um\tvoltage_v\n";
    for (const pdn::VoltageSource& source : problem.voltage_sources) {
      out << source.layer << '\t' << source.region_um.llx_um << '\t' << source.region_um.lly_um << '\t'
          << source.region_um.urx_um << '\t' << source.region_um.ury_um << '\t' << source.voltage_v << '\n';
    }
  }

  {
    std::ofstream out(dump_dir / "point_loads.tsv");
    out << "name\tlayer\tx_um\ty_um\tcurrent_a\n";
    for (const pdn::PointLoad& load : problem.point_loads) {
      out << load.name << '\t' << load.layer << '\t' << load.location_um.x_um << '\t' << load.location_um.y_um
          << '\t' << load.current_a << '\n';
    }
  }

  {
    std::ofstream out(dump_dir / "distributed_loads.tsv");
    out << "name\tlayer\tllx_um\tlly_um\turx_um\tury_um\ttotal_current_a\n";
    for (const pdn::DistributedLoad& load : problem.distributed_loads) {
      out << load.name << '\t' << load.layer << '\t' << load.region_um.llx_um << '\t' << load.region_um.lly_um
          << '\t' << load.region_um.urx_um << '\t' << load.region_um.ury_um << '\t' << load.total_current_a
          << '\n';
    }
  }

  {
    std::ofstream out(dump_dir / "meta.tsv");
    out << "key\tvalue\n";
    out << "core_llx_um\t" << problem.core_um.llx_um << '\n';
    out << "core_lly_um\t" << problem.core_um.lly_um << '\n';
    out << "core_urx_um\t" << problem.core_um.urx_um << '\n';
    out << "core_ury_um\t" << problem.core_um.ury_um << '\n';
    out << "observation_layer\t" << problem.observation_layer << '\n';
    out << "observation_pitch_um\t" << problem.observation_pitch_um << '\n';
    out << "min_voltage_v\t" << result.min_voltage_v << '\n';
    out << "max_voltage_v\t" << result.max_voltage_v << '\n';
    out << "iterations\t" << result.iterations << '\n';
    out << "residual_l2\t" << result.residual_l2 << '\n';
  }
}

}  // namespace

int main(int argc, char** argv)
{
  try {
    std::filesystem::path input = std::filesystem::is_regular_file("../research/mempool.json")
                                      ? std::filesystem::path("../research/mempool.json")
                                      : std::filesystem::path("examples/example_problem.json");
    std::filesystem::path dump_dir = "out/latest";
    if (argc >= 2 && std::string(argv[1]) != "--dump-dir") {
      input = argv[1];
    }
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--dump-dir") {
        if (i + 1 >= argc) {
          throw std::runtime_error("--dump-dir requires a path.");
        }
        dump_dir = argv[i + 1];
        ++i;
      }
    }
    if (!std::filesystem::is_regular_file(input)) {
      printUsage(argc >= 1 ? argv[0] : "pdn_demo");
      throw std::runtime_error("Input file not found: " + input.string());
    }

    const pdn::PdnProblem problem = pdn::loadProblemFromJsonFile(input);
    const pdn::ResistiveNetwork network = pdn::PdnBuilder::build(problem);
    const pdn::SolveResult result = pdn::DcSolver::solve(network);

    const pdn::LayerLattice& observation = network.observationLattice();
    double observation_min_v = std::numeric_limits<double>::infinity();
    double observation_max_v = -std::numeric_limits<double>::infinity();
    int fixed_nodes = 0;
    double total_load_a = 0.0;
    int via_edges = 0;
    for (const pdn::Node& node : network.nodes) {
      if (node.fixed_voltage) {
        ++fixed_nodes;
      }
      total_load_a += -std::min(0.0, node.injected_current_a);
      if (node.layer_index == observation.layer_index) {
        const double v = result.node_voltages_v.at(static_cast<std::size_t>(node.id));
        observation_min_v = std::min(observation_min_v, v);
        observation_max_v = std::max(observation_max_v, v);
      }
    }
    for (const pdn::Edge& edge : network.edges) {
      if (edge.kind == pdn::EdgeKind::kVia) {
        ++via_edges;
      }
    }

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "PDN problem solved\n";
    std::cout << "  input: " << input.string() << '\n';
    std::cout << "  layers: " << network.lattices.size() << '\n';
    std::cout << "  nodes: " << network.nodes.size() << '\n';
    std::cout << "  edges: " << network.edges.size() << " (" << via_edges << " via, "
              << (network.edges.size() - static_cast<std::size_t>(via_edges)) << " wire)\n";
    std::cout << "  fixed-voltage nodes: " << fixed_nodes << '\n';
    std::cout << "  total load current: " << total_load_a << " A\n";
    std::cout << "  solver iterations: " << result.iterations << '\n';
    std::cout << "  global voltage range: [" << result.min_voltage_v << ", " << result.max_voltage_v
              << "] V\n";
    std::cout << "  observation layer: " << observation.layer_name << '\n';
    std::cout << "  observation lattice: " << observation.nx << "x" << observation.ny
              << " pitch=" << observation.pitch_x_um << " um\n";
    std::cout << "  observation voltage range: [" << observation_min_v << ", "
              << observation_max_v << "] V\n";

    const std::size_t preview_count = std::min<std::size_t>(5, network.edges.size());
    std::cout << "  first " << preview_count << " edges:\n";
    for (std::size_t i = 0; i < preview_count; ++i) {
      const pdn::Edge& edge = network.edges[i];
      std::cout << "    " << edge.from << " -> " << edge.to << "  kind=" << edgeKindName(edge.kind)
                << "  R=" << edge.resistance_ohm << " ohm\n";
    }
    dumpTsv(dump_dir, problem, network, result);
    std::cout << "  dump dir: " << dump_dir.string() << '\n';

    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
