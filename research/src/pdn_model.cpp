#include <phys/pdn_model.hpp>
#include <phys/pdn_extract.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

namespace phys {
namespace {

struct ViaRow {
  double x{}, y{};
  string lo, hi;
};

struct LayerObs {
  int wireCount{};
  vector<double> xw;
  vector<double> yw;
  vector<double> hTracks;
  vector<double> vTracks;
  vector<double> declaredPitch;
};

vector<string> splitTab(const string& line) {
  vector<string> out;
  size_t p = 0;
  while (true) {
    size_t q = line.find('\t', p);
    out.push_back(q == string::npos ? line.substr(p) : line.substr(p, q - p));
    if (q == string::npos)
      break;
    p = q + 1;
  }
  return out;
}

vector<ViaRow> readViaRows(const fs::path& path) {
  ifstream in(path);
  if (!in)
    throw runtime_error("pdn_vias_file: cannot read: " + path.string());
  vector<ViaRow> vias;
  string line;
  while (getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    vector<string> tok = splitTab(line);
    if (tok.size() < 8)
      continue;
    if (tok[0] == "x_um")
      continue;
    ViaRow v;
    v.x = stod(tok[0]);
    v.y = stod(tok[1]);
    v.lo = tok[6];
    v.hi = tok[7];
    vias.push_back(move(v));
  }
  return vias;
}

int metalRank(const string& layer) {
  int rank = -1, run = -1;
  for (char c : layer) {
    if (isdigit(static_cast<unsigned char>(c))) {
      if (run < 0)
        run = 0;
      run = run * 10 + (c - '0');
      rank = run;
    } else {
      run = -1;
    }
  }
  return rank;
}

string pickPowerNet(const Chip& chip) {
  if (!chip.tech().voltageDomains().empty() && !chip.tech().voltageDomains().front().power_net.empty())
    return chip.tech().voltageDomains().front().power_net;
  return "VDD";
}

bool pdnDebugEnabled() {
  const char* v = std::getenv("PHYS_PDN_DEBUG");
  return v != nullptr && string(v) == "1";
}

string pdnParseSource() {
  const char* v = std::getenv("PHYS_PDN_PARSE_SOURCE");
  if (v == nullptr || string(v).empty())
    return "tcl";
  string s = v;
  transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
  if (s == "odb" || s == "tcl")
    return s;
  throw runtime_error("pdn: PHYS_PDN_PARSE_SOURCE must be 'odb' or 'tcl'");
}

double median(vector<double> v) {
  if (v.empty())
    return 0.0;
  sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double pitchFrom(vector<double> coords) {
  if (coords.size() < 2)
    return 0.0;
  sort(coords.begin(), coords.end());
  coords.erase(unique(coords.begin(), coords.end(), [](double a, double b) { return fabs(a - b) < 1e-6; }), coords.end());
  vector<double> delta;
  for (size_t i = 1; i < coords.size(); ++i) {
    const double d = coords[i] - coords[i - 1];
    if (d > 1e-6)
      delta.push_back(d);
  }
  return median(delta);
}

unordered_map<string, LayerObs> readShapes(const Chip& chip, const unordered_map<string, double>& rPerUm) {
  if (chip.pdnShapesFile().empty())
    throw runtime_error("pdn: pdn_shapes_file is required");
  unordered_map<string, LayerObs> byLayer;
  ifstream in(chip.pdnShapesFile());
  if (!in)
    throw runtime_error("pdn: cannot read pdn_shapes_file: " + chip.pdnShapesFile());
  const string powerNet = pickPowerNet(chip);
  if (pdnDebugEnabled())
    cerr << "[pdn-model] net filter: " << powerNet << '\n';
  string header;
  getline(in, header);
  string line;
  while (getline(in, line)) {
    if (line.empty())
      continue;
    vector<string> tok = splitTab(line);
    // net, sig_type, shape_kind, layer, llx, lly, urx, ury, cx, cy
    if (tok.size() < 10 || tok[2] != "wire")
      continue;
    if (tok[1] != "POWER")
      continue;
    if (tok[0] != powerNet)
      continue;
    const string& layer = tok[3];
    if (!rPerUm.count(layer))
      continue;
    const double llx = stod(tok[4]), lly = stod(tok[5]), urx = stod(tok[6]), ury = stod(tok[7]);
    const double dx = max(0.0, urx - llx), dy = max(0.0, ury - lly);
    auto& obs = byLayer[layer];
    if (dx >= dy) {
      if (dy > 0.0)
        obs.yw.push_back(dy);
      obs.hTracks.push_back((lly + ury) * 0.5);
    } else {
      if (dx > 0.0)
        obs.xw.push_back(dx);
      obs.vTracks.push_back((llx + urx) * 0.5);
    }
  }
  if (pdnDebugEnabled()) {
    for (const auto& kv : byLayer) {
      cerr << "[pdn-model] layer_obs " << kv.first << " hTracks=" << kv.second.hTracks.size()
           << " vTracks=" << kv.second.vTracks.size() << '\n';
    }
  }
  return byLayer;
}

unordered_map<string, LayerObs> readStripesFromTcl(const Chip& chip,
                                                   const unordered_map<string, double>& rPerUm) {
  unordered_map<string, LayerObs> byLayer;
  int minRank = numeric_limits<int>::max();
  for (const auto& s : chip.tech().stripes()) {
    if (s.width <= 0.0 || s.pitch <= 0.0 || !rPerUm.count(s.layer))
      continue;
    const int r = metalRank(s.layer);
    if (r > 0)
      minRank = min(minRank, r);
  }
  const bool haveMinRank = (minRank != numeric_limits<int>::max());
  for (const auto& s : chip.tech().stripes()) {
    if (s.width <= 0.0 || s.pitch <= 0.0)
      continue;
    if (!rPerUm.count(s.layer))
      continue;
    // PDNSim-style intent: keep fine bottom-layer discretization (often followpins rails),
    // but avoid injecting all followpins layers.
    if (s.followpins) {
      const int r = metalRank(s.layer);
      if (!(haveMinRank && r == minRank))
        continue;
    }
    auto& obs = byLayer[s.layer];
    obs.wireCount += 1;
    obs.declaredPitch.push_back(s.pitch);
    // No exact coordinates in Tcl; keep abstract tracks by pitch.
    // Use metal rank parity heuristic for direction (odd H, even V).
    const int rank = metalRank(s.layer);
    const bool isHoriz = (rank > 0) ? (rank % 2 == 1) : true;
    if (isHoriz) {
      obs.yw.push_back(s.width);
      obs.hTracks.push_back(s.pitch * static_cast<double>(obs.wireCount));
    } else {
      obs.xw.push_back(s.width);
      obs.vTracks.push_back(s.pitch * static_cast<double>(obs.wireCount));
    }
  }
  return byLayer;
}

vector<PdnModel::Layer> makeLayers(const unordered_map<string, LayerObs>& byLayer,
                                   const unordered_map<string, double>& rPerUm,
                                   double minPitchUm) {
  vector<PdnModel::Layer> layers;
  for (const auto& kv : byLayer) {
    const string& layer = kv.first;
    const LayerObs& obs = kv.second;
    const bool isHoriz = obs.hTracks.size() >= obs.vTracks.size();
    const double width = isHoriz ? median(obs.yw) : median(obs.xw);
    double pitch = isHoriz ? pitchFrom(obs.hTracks) : pitchFrom(obs.vTracks);
    if (pitch <= 0.0 && !obs.declaredPitch.empty())
      pitch = median(obs.declaredPitch);
    if (pdnDebugEnabled())
      cerr << "[pdn-model] layer_fit " << layer << " dir=" << (isHoriz ? "H" : "V")
           << " pitch=" << pitch << " width=" << width << '\n';
    if (width <= 0.0 || pitch < minPitchUm)
      continue;
    const double rSh = rPerUm.at(layer) * width;
    const double seg = pitch / width;
    const double R = (rSh > 0.0 && seg > 0.0) ? rSh * seg : 0.0;
    const double gSeg = (R > 0.0) ? (1.0 / R) : 0.0;
    layers.push_back({layer, isHoriz, pitch, width, gSeg, metalRank(layer)});
  }
  sort(layers.begin(), layers.end(), [](const auto& a, const auto& b) { return a.pitchUm < b.pitchUm; });
  return layers;
}

vector<pair<int, int>> makeViasFromOdbTsv(const Chip& chip, const vector<PdnModel::Layer>& layers) {
  if (chip.pdnViasFile().empty())
    throw runtime_error("pdn: PHYS_PDN_PARSE_SOURCE=odb requires pdn_vias_file");
  const fs::path path = fs::weakly_canonical(chip.repoRoot() / chip.pdnViasFile());
  if (!fs::is_regular_file(path))
    throw runtime_error("pdn: cannot read pdn_vias_file: " + path.string());

  unordered_map<string, int> layerToIx;
  for (int i = 0; i < static_cast<int>(layers.size()); ++i)
    layerToIx[layers[static_cast<size_t>(i)].name] = i;

  set<pair<int, int>> uniquePairs;
  for (const auto& row : readViaRows(path)) {
    auto a = layerToIx.find(row.lo), b = layerToIx.find(row.hi);
    if (a == layerToIx.end() || b == layerToIx.end() || a->second == b->second)
      continue;
    int ia = a->second, ib = b->second;
    if (ia > ib)
      swap(ia, ib);
    uniquePairs.emplace(ia, ib);
  }
  return vector<pair<int, int>>(uniquePairs.begin(), uniquePairs.end());
}

/// Layer pairs from `add_pdn_connect` in pdn_tcl (first two layers per call).
vector<pair<int, int>> makeViasFromPdnTcl(const Chip& chip, const vector<PdnModel::Layer>& layers) {
  unordered_map<string, int> layerToIx;
  for (int i = 0; i < static_cast<int>(layers.size()); ++i)
    layerToIx[layers[static_cast<size_t>(i)].name] = i;

  set<pair<int, int>> uniquePairs;
  for (const auto& c : chip.tech().pdnConnects()) {
    auto a = layerToIx.find(c.layer_lower), b = layerToIx.find(c.layer_upper);
    if (a == layerToIx.end() || b == layerToIx.end() || a->second == b->second)
      continue;
    int ia = a->second, ib = b->second;
    if (ia > ib)
      swap(ia, ib);
    uniquePairs.emplace(ia, ib);
  }
  return vector<pair<int, int>>(uniquePairs.begin(), uniquePairs.end());
}

void logPdnChecks(const Chip& chip, const PdnModel& model) {
  cout << "[pdn-check] layers=" << model.layers.size() << " via_pairs=" << model.viaPairs.size() << "\n";
  unordered_map<int, string> ixToName;
  unordered_map<string, int> nameToIx;
  for (size_t i = 0; i < model.layers.size(); ++i) {
    ixToName[static_cast<int>(i)] = model.layers[i].name;
    nameToIx[model.layers[i].name] = static_cast<int>(i);
  }

  int validPairs = 0;
  for (const auto& p : model.viaPairs) {
    const bool ok = ixToName.count(p.first) && ixToName.count(p.second) && p.first != p.second;
    if (ok)
      ++validPairs;
  }
  cout << "[pdn-check] via_pair_index_valid=" << validPairs << "/" << model.viaPairs.size() << "\n";

  set<pair<string, string>> observed;
  for (const auto& p : model.viaPairs) {
    if (!ixToName.count(p.first) || !ixToName.count(p.second))
      continue;
    string a = ixToName[p.first], b = ixToName[p.second];
    if (a > b)
      swap(a, b);
    observed.emplace(a, b);
  }

  int tclConnectPresent = 0;
  int tclConnectKnown = 0;
  for (const auto& c : chip.tech().pdnConnects()) {
    string a = c.layer_lower, b = c.layer_upper;
    if (a > b)
      swap(a, b);
    if (!nameToIx.count(a) || !nameToIx.count(b))
      continue;
    ++tclConnectKnown;
    if (observed.count({a, b}))
      ++tclConnectPresent;
  }
  if (tclConnectKnown > 0) {
    cout << "[pdn-check] tcl_connect_with_vias=" << tclConnectPresent << "/" << tclConnectKnown << "\n";
  } else {
    cout << "[pdn-check] tcl_connect_with_vias=NA (no overlapping connect pairs)\n";
  }
}

}  // namespace

PdnModel buildPdnModel(const Chip& chip) {
  unordered_map<string, double> rPerUm;
  for (const auto& t : chip.tech().layerResistanceCapacitance())
    rPerUm[t.layer] = t.r;
  PdnModel model;
  const string source = pdnParseSource();
  const auto layerObs = (source == "tcl") ? readStripesFromTcl(chip, rPerUm) : readShapes(chip, rPerUm);
  // ODB graph path: filter out very fine followpins-like layers (e.g. metal1)
  // to keep graph abstraction/runtime tractable.
  const double minPitchUm = (source == "tcl") ? 0.0 : 5.0;
  model.layers = makeLayers(layerObs, rPerUm, minPitchUm);
  model.viaPairs = (source == "tcl") ? makeViasFromPdnTcl(chip, model.layers) : makeViasFromOdbTsv(chip, model.layers);
  cout << "[pdn] parse_source=" << source << "\n";
  logPdnChecks(chip, model);
  const char* emit = std::getenv("PHYS_PDN_EXTRACT_REPORT");
  if (emit != nullptr && string(emit) != "0") {
    if (!chip.pdnShapesFile().empty() && !chip.pdnViasFile().empty()) {
      const auto summary = buildPdnExtractSummary(chip);
      const fs::path out = chip.repoRoot() / "research/out/pdn_extract_report.tsv";
      fs::create_directories(out.parent_path());
      writePdnExtractSummaryTsv(summary, out.string());
    } else {
      cout << "[pdn] PHYS_PDN_EXTRACT_REPORT skipped (needs pdn_shapes_file and pdn_vias_file on disk)\n";
    }
  }
  return model;
}

PdnModel buildPdnModelPdnsimStyle(const Chip& chip) {
  // Historically "PDNSim-style" meant DB shapes; set PHYS_PDN_PARSE_SOURCE=odb for that.
  return buildPdnModel(chip);
}

}  // namespace phys
