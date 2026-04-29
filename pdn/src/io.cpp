#include <pdn/io.hpp>

#include <pdn/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdn {
namespace {

const json::Value& requireObjectField(const json::Value& object, const std::string& key)
{
  if (!object.isObject()) {
    throw std::runtime_error("Expected JSON object.");
  }
  return object.at(key);
}

double requireNumber(const json::Value& object, const std::string& key)
{
  return requireObjectField(object, key).asNumber();
}

std::string requireString(const json::Value& object, const std::string& key)
{
  return requireObjectField(object, key).asString();
}

double optionalNumber(const json::Value& object, const std::string& key, double default_value)
{
  if (!object.contains(key)) {
    return default_value;
  }
  return object.at(key).asNumber();
}

Rect parseRect(const json::Value& value)
{
  const auto& array = value.asArray();
  if (array.size() != 4) {
    throw std::runtime_error("Rectangle array must contain exactly 4 numbers.");
  }
  return {array[0].asNumber(), array[1].asNumber(), array[2].asNumber(), array[3].asNumber()};
}

Point parsePoint(const json::Value& value)
{
  const auto& array = value.asArray();
  if (array.size() != 2) {
    throw std::runtime_error("Point array must contain exactly 2 numbers.");
  }
  return {array[0].asNumber(), array[1].asNumber()};
}

Orientation parseOrientation(const std::string& text)
{
  if (text == "horizontal") {
    return Orientation::kHorizontal;
  }
  if (text == "vertical") {
    return Orientation::kVertical;
  }
  throw std::runtime_error("Orientation must be 'horizontal' or 'vertical'.");
}

std::string readTextFile(const std::filesystem::path& path)
{
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot read file: " + path.string());
  }
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::vector<std::string> splitTab(const std::string& line)
{
  std::vector<std::string> tokens;
  std::size_t start = 0;
  while (true) {
    std::size_t pos = line.find('\t', start);
    tokens.push_back(pos == std::string::npos ? line.substr(start) : line.substr(start, pos - start));
    if (pos == std::string::npos) {
      break;
    }
    start = pos + 1;
  }
  return tokens;
}

std::vector<std::string> splitSpace(const std::string& line)
{
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string tok;
  while (ss >> tok) {
    out.push_back(tok);
  }
  return out;
}

int layerRank(const std::string& layer)
{
  int best = -1;
  int run = -1;
  for (char c : layer) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      if (run < 0) {
        run = 0;
      }
      run = run * 10 + (c - '0');
      best = run;
    } else {
      run = -1;
    }
  }
  return best;
}

double median(std::vector<double> values)
{
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

std::filesystem::path findRepoRoot(std::filesystem::path from)
{
  if (std::filesystem::is_regular_file(from)) {
    from = from.parent_path();
  }
  while (!from.empty()) {
    if (std::filesystem::is_directory(from / "flow")
        && std::filesystem::is_directory(from / "research")) {
      return from;
    }
    const std::filesystem::path parent = from.parent_path();
    if (parent == from) {
      break;
    }
    from = parent;
  }
  throw std::runtime_error("Could not locate repo root from manifest path.");
}

std::filesystem::path resolveFromRepoRoot(const std::filesystem::path& repo_root, const std::string& rel)
{
  return std::filesystem::weakly_canonical(repo_root / rel);
}

bool isDirectSchema(const json::Value& root)
{
  return root.contains("core_um");
}

PdnProblem loadDirectSchema(const json::Value& root)
{
  PdnProblem problem;
  problem.core_um = parseRect(root.at("core_um"));
  problem.observation_layer = requireString(root, "observation_layer");
  problem.observation_pitch_um = optionalNumber(root, "observation_pitch_um", 0.0);

  for (const json::Value& layer_value : root.at("layers").asArray()) {
    MetalLayer layer;
    layer.name = requireString(layer_value, "name");
    layer.orientation = parseOrientation(requireString(layer_value, "orientation"));
    layer.rank = static_cast<int>(requireNumber(layer_value, "rank"));
    layer.pitch_um = requireNumber(layer_value, "pitch_um");
    layer.width_um = requireNumber(layer_value, "width_um");
    layer.sheet_resistance_ohm_per_square
        = requireNumber(layer_value, "sheet_resistance_ohm_per_square");
    problem.layers.push_back(layer);
  }

  if (root.contains("via_connections")) {
    for (const json::Value& via_value : root.at("via_connections").asArray()) {
      ViaConnection via;
      via.lower_layer = requireString(via_value, "lower_layer");
      via.upper_layer = requireString(via_value, "upper_layer");
      via.resistance_ohm = requireNumber(via_value, "resistance_ohm");
      problem.via_connections.push_back(via);
    }
  }

  if (root.contains("voltage_sources")) {
    for (const json::Value& source_value : root.at("voltage_sources").asArray()) {
      VoltageSource source;
      source.layer = requireString(source_value, "layer");
      source.region_um = parseRect(source_value.at("region_um"));
      source.voltage_v = requireNumber(source_value, "voltage_v");
      problem.voltage_sources.push_back(source);
    }
  }

  if (root.contains("point_loads")) {
    for (const json::Value& load_value : root.at("point_loads").asArray()) {
      PointLoad load;
      load.name = requireString(load_value, "name");
      load.layer = requireString(load_value, "layer");
      load.location_um = parsePoint(load_value.at("location_um"));
      load.current_a = requireNumber(load_value, "current_a");
      problem.point_loads.push_back(load);
    }
  }

  if (root.contains("distributed_loads")) {
    for (const json::Value& load_value : root.at("distributed_loads").asArray()) {
      DistributedLoad load;
      load.name = requireString(load_value, "name");
      load.layer = requireString(load_value, "layer");
      load.region_um = parseRect(load_value.at("region_um"));
      load.total_current_a = requireNumber(load_value, "total_current_a");
      problem.distributed_loads.push_back(load);
    }
  }
  return problem;
}

PdnProblem loadMempoolSchema(const json::Value& root, const std::filesystem::path& manifest_path)
{
  const std::filesystem::path repo_root = findRepoRoot(manifest_path);
  PdnProblem problem;
  problem.core_um = parseRect(root.at("layout").at("core"));

  std::unordered_map<std::string, double> rc_by_layer;
  {
    const std::filesystem::path set_rc
        = resolveFromRepoRoot(repo_root, root.at("tech").at("set_rc_tcl").asString());
    std::ifstream in(set_rc);
    if (!in) {
      throw std::runtime_error("Cannot read set_rc_tcl: " + set_rc.string());
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("set_layer_rc") == std::string::npos) {
        continue;
      }
      const std::vector<std::string> tok = splitSpace(line);
      std::string layer;
      double resistance = 0.0;
      for (std::size_t i = 0; i + 1 < tok.size(); ++i) {
        if (tok[i] == "-layer") {
          layer = tok[i + 1];
        } else if (tok[i] == "-resistance") {
          resistance = std::stod(tok[i + 1]);
        }
      }
      if (!layer.empty() && resistance > 0.0) {
        rc_by_layer[layer] = resistance;
      }
    }
  }

  struct LayerObs {
    std::vector<double> width;
    std::vector<double> pitch;
    int horiz_votes {};
  };
  std::unordered_map<std::string, LayerObs> layer_obs;
  {
    const std::filesystem::path tsv = resolveFromRepoRoot(repo_root, "research/out/pdn_tcl_physical.tsv");
    std::ifstream in(tsv);
    if (!in) {
      throw std::runtime_error("Cannot read pdn_tcl_physical.tsv: " + tsv.string());
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const std::vector<std::string> tok = splitTab(line);
      if (tok.size() < 13 || tok[0] != "strap") {
        continue;
      }
      LayerObs& obs = layer_obs[tok[2]];
      obs.width.push_back(std::stod(tok[4]));
      obs.pitch.push_back(std::stod(tok[5]));
      obs.horiz_votes += std::stoi(tok[8]) ? 1 : -1;
    }
  }

  for (const auto& kv : layer_obs) {
    if (!rc_by_layer.count(kv.first)) {
      continue;
    }
    MetalLayer l;
    l.name = kv.first;
    l.orientation = kv.second.horiz_votes >= 0 ? Orientation::kHorizontal : Orientation::kVertical;
    l.rank = layerRank(kv.first);
    l.pitch_um = median(kv.second.pitch);
    l.width_um = median(kv.second.width);
    l.sheet_resistance_ohm_per_square = rc_by_layer[kv.first];
    if (l.pitch_um >= 5.0 && l.width_um > 0.0) {
      problem.layers.push_back(l);
    }
  }
  std::sort(problem.layers.begin(), problem.layers.end(), [](const MetalLayer& a, const MetalLayer& b) {
    return a.rank < b.rank;
  });
  if (problem.layers.empty()) {
    throw std::runtime_error("No PDN layers available from mempool inputs.");
  }

  {
    const std::filesystem::path vias
        = resolveFromRepoRoot(repo_root, root.at("pdn_vias_file").asString());
    std::ifstream in(vias);
    if (!in) {
      throw std::runtime_error("Cannot read pdn_vias_file: " + vias.string());
    }
    std::map<std::pair<std::string, std::string>, int> via_pairs;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const auto tok = splitTab(line);
      if (tok.size() < 8 || tok[0] == "x_um") {
        continue;
      }
      std::string a = tok[6];
      std::string b = tok[7];
      if (a > b) {
        std::swap(a, b);
      }
      via_pairs[{a, b}]++;
    }
    std::unordered_map<std::string, int> layer_present;
    for (const auto& layer : problem.layers) {
      layer_present[layer.name] = 1;
    }
    for (const auto& kv : via_pairs) {
      if (!layer_present.count(kv.first.first) || !layer_present.count(kv.first.second)) {
        continue;
      }
      problem.via_connections.push_back({kv.first.first, kv.first.second, 0.01});
    }
  }

  {
    const std::filesystem::path vsrc
        = resolveFromRepoRoot(repo_root, root.at("psm_vsrc_boxes_file").asString());
    std::ifstream in(vsrc);
    if (!in) {
      throw std::runtime_error("Cannot read psm_vsrc_boxes_file: " + vsrc.string());
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const auto tok = splitTab(line);
      if (tok.size() < 5 || tok[0] == "llx_um") {
        continue;
      }
      VoltageSource s;
      s.region_um = {std::stod(tok[0]), std::stod(tok[1]), std::stod(tok[2]), std::stod(tok[3])};
      s.voltage_v = std::stod(tok[4]);
      s.layer = tok.size() > 5 ? tok[5] : problem.layers.back().name;
      problem.voltage_sources.push_back(s);
    }
  }

  std::unordered_map<std::string, double> current_by_instance;
  {
    const std::filesystem::path pwr
        = resolveFromRepoRoot(repo_root, root.at("report_power_instances_tsv").asString());
    std::ifstream in(pwr);
    if (!in) {
      throw std::runtime_error("Cannot read report_power_instances_tsv: " + pwr.string());
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      const auto tok = splitTab(line);
      if (tok.size() < 6 || tok[0] == "instance") {
        continue;
      }
      current_by_instance[tok[0]] = std::stod(tok[5]);
    }
  }

  std::unordered_map<std::string, std::string> cluster_by_instance;
  {
    const std::filesystem::path groups = resolveFromRepoRoot(repo_root, root.at("groups").asString());
    std::ifstream in(groups);
    if (!in) {
      throw std::runtime_error("Cannot read groups file: " + groups.string());
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const auto tok = splitSpace(line);
      if (tok.size() < 3 || tok[0] == "instance") {
        continue;
      }
      cluster_by_instance[tok[0]] = tok[2];
    }
  }

  struct ClusterAgg {
    double llx {std::numeric_limits<double>::infinity()};
    double lly {std::numeric_limits<double>::infinity()};
    double urx {-std::numeric_limits<double>::infinity()};
    double ury {-std::numeric_limits<double>::infinity()};
    double current_a {};
  };
  std::unordered_map<std::string, ClusterAgg> clusters;
  std::unordered_map<std::string, int> is_macro;
  std::unordered_map<std::string, Point> center;
  std::unordered_map<std::string, std::vector<Point>> pg_pins;
  {
    const std::filesystem::path geom
        = resolveFromRepoRoot(repo_root, root.at("instance_geom_tsv").asString());
    std::ifstream in(geom);
    if (!in) {
      throw std::runtime_error("Cannot read instance_geom_tsv: " + geom.string());
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      const auto tok = splitTab(line);
      if (tok.size() < 10 || tok[0] == "instance") {
        continue;
      }
      const std::string& inst = tok[0];
      const double llx = std::stod(tok[3]);
      const double lly = std::stod(tok[4]);
      const double urx = std::stod(tok[5]);
      const double ury = std::stod(tok[6]);
      center[inst] = {std::stod(tok[7]), std::stod(tok[8])};
      is_macro[inst] = std::stoi(tok[2]);
      if (tok.size() >= 12 && !tok[10].empty() && !tok[11].empty()) {
        pg_pins[inst].push_back({std::stod(tok[10]), std::stod(tok[11])});
      }

      auto c_it = cluster_by_instance.find(inst);
      auto p_it = current_by_instance.find(inst);
      if (c_it == cluster_by_instance.end() || p_it == current_by_instance.end()) {
        continue;
      }
      ClusterAgg& agg = clusters[c_it->second];
      agg.llx = std::min(agg.llx, llx);
      agg.lly = std::min(agg.lly, lly);
      agg.urx = std::max(agg.urx, urx);
      agg.ury = std::max(agg.ury, ury);
      agg.current_a += p_it->second;
    }
  }

  int best_layer = 0;
  for (std::size_t i = 0; i < problem.layers.size(); ++i) {
    if (problem.layers[i].rank > 1
        && (problem.layers[i].rank < problem.layers[best_layer].rank || problem.layers[best_layer].rank <= 1)) {
      best_layer = static_cast<int>(i);
    }
  }
  problem.observation_layer = problem.layers[static_cast<std::size_t>(best_layer)].name;
  problem.observation_pitch_um = problem.layers[static_cast<std::size_t>(best_layer)].pitch_um;

  for (const auto& kv : clusters) {
    if (!std::isfinite(kv.second.llx) || kv.second.current_a <= 0.0) {
      continue;
    }
    problem.distributed_loads.push_back(
        {kv.first,
         problem.observation_layer,
         {kv.second.llx, kv.second.lly, kv.second.urx, kv.second.ury},
         kv.second.current_a});
  }

  int pin_idx = 0;
  for (const auto& kv : current_by_instance) {
    if (!is_macro.count(kv.first) || !is_macro[kv.first] || kv.second <= 0.0) {
      continue;
    }
    auto pins = pg_pins[kv.first];
    if (pins.empty()) {
      pins.push_back(center[kv.first]);
    }
    const double i_per_pin = kv.second / static_cast<double>(pins.size());
    for (const Point& p : pins) {
      PointLoad load;
      load.name = kv.first + "#pg" + std::to_string(pin_idx++);
      load.layer = problem.observation_layer;
      load.location_um = p;
      load.current_a = i_per_pin;
      problem.point_loads.push_back(load);
    }
  }

  return problem;
}

}  // namespace

PdnProblem loadProblemFromJsonFile(const std::filesystem::path& path)
{
  const json::Value root = json::parse(readTextFile(path));
  if (!root.isObject()) {
    throw std::runtime_error("Top-level JSON value must be an object.");
  }

  if (isDirectSchema(root)) {
    return loadDirectSchema(root);
  }
  return loadMempoolSchema(root, path);
}

}  // namespace pdn
