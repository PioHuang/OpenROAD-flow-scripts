#include <phys/pdn_extract.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

namespace phys {
namespace {

vector<string> splitTab(const string& line) {
  vector<string> out;
  size_t p = 0;
  while (true) {
    const size_t q = line.find('\t', p);
    out.push_back(q == string::npos ? line.substr(p) : line.substr(p, q - p));
    if (q == string::npos)
      break;
    p = q + 1;
  }
  return out;
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

string pickPowerNet(const Chip& chip) {
  if (!chip.tech().voltageDomains().empty() && !chip.tech().voltageDomains().front().power_net.empty())
    return chip.tech().voltageDomains().front().power_net;
  return "VDD";
}

}  // namespace

PdnExtractSummary buildPdnExtractSummary(const Chip& chip) {
  if (chip.pdnShapesFile().empty())
    throw runtime_error("pdn_extract: pdn_shapes_file is required");
  if (chip.pdnViasFile().empty())
    throw runtime_error("pdn_extract: pdn_vias_file is required");

  unordered_map<string, double> rPerUm;
  for (const auto& t : chip.tech().layerResistanceCapacitance())
    rPerUm[t.layer] = t.r;

  struct Obs {
    int wireCount{};
    vector<double> xw;
    vector<double> yw;
    vector<double> hTracks;
    vector<double> vTracks;
  };
  unordered_map<string, Obs> byLayer;

  const string powerNet = pickPowerNet(chip);
  ifstream in(chip.pdnShapesFile());
  if (!in)
    throw runtime_error("pdn_extract: cannot read pdn_shapes_file: " + chip.pdnShapesFile());
  string line;
  getline(in, line);
  while (getline(in, line)) {
    if (line.empty())
      continue;
    const vector<string> tok = splitTab(line);
    if (tok.size() < 10 || tok[2] != "wire" || tok[1] != "POWER" || tok[0] != powerNet)
      continue;
    const string& layer = tok[3];
    if (!rPerUm.count(layer))
      continue;
    const double llx = stod(tok[4]), lly = stod(tok[5]), urx = stod(tok[6]), ury = stod(tok[7]);
    const double dx = max(0.0, urx - llx), dy = max(0.0, ury - lly);
    auto& o = byLayer[layer];
    o.wireCount++;
    if (dx >= dy) {
      if (dy > 0.0)
        o.yw.push_back(dy);
      o.hTracks.push_back((lly + ury) * 0.5);
    } else {
      if (dx > 0.0)
        o.xw.push_back(dx);
      o.vTracks.push_back((llx + urx) * 0.5);
    }
  }

  PdnExtractSummary out;
  out.powerNet = powerNet;
  for (const auto& kv : byLayer) {
    const string& layer = kv.first;
    const Obs& o = kv.second;
    const bool isHoriz = o.hTracks.size() >= o.vTracks.size();
    const double width = isHoriz ? median(o.yw) : median(o.xw);
    const double pitch = isHoriz ? pitchFrom(o.hTracks) : pitchFrom(o.vTracks);
    out.layers.push_back({layer,
                          o.wireCount,
                          static_cast<int>(o.hTracks.size()),
                          static_cast<int>(o.vTracks.size()),
                          pitch,
                          width});
  }
  sort(out.layers.begin(), out.layers.end(), [](const auto& a, const auto& b) { return a.pitchUm < b.pitchUm; });

  set<pair<string, string>> uniqueLinks;
  const fs::path viaPath = fs::weakly_canonical(chip.repoRoot() / chip.pdnViasFile());
  ifstream vin(viaPath);
  if (!vin)
    throw runtime_error("pdn_extract: cannot read pdn_vias_file: " + viaPath.string());
  getline(vin, line);
  while (getline(vin, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    const vector<string> tok = splitTab(line);
    if (tok.size() < 8 || tok[0] == "x_um")
      continue;
    string a = tok[6], b = tok[7];
    if (a == b)
      continue;
    if (a > b)
      swap(a, b);
    uniqueLinks.emplace(a, b);
  }
  out.viaLinks.assign(uniqueLinks.begin(), uniqueLinks.end());

  unordered_map<string, int> id;
  for (size_t i = 0; i < out.layers.size(); ++i)
    id[out.layers[i].name] = static_cast<int>(i);
  vector<int> p(out.layers.size());
  iota(p.begin(), p.end(), 0);
  auto f = [&](int x) {
    int r = x;
    while (p[r] != r)
      r = p[r];
    while (p[x] != x) {
      int n = p[x];
      p[x] = r;
      x = n;
    }
    return r;
  };
  auto u = [&](int a, int b) {
    a = f(a);
    b = f(b);
    if (a != b)
      p[b] = a;
  };
  for (const auto& e : out.viaLinks) {
    auto ia = id.find(e.first), ib = id.find(e.second);
    if (ia != id.end() && ib != id.end())
      u(ia->second, ib->second);
  }
  set<int> roots;
  for (size_t i = 0; i < out.layers.size(); ++i)
    roots.insert(f(static_cast<int>(i)));
  out.connectedComponents = static_cast<int>(roots.size());
  return out;
}

void writePdnExtractSummaryTsv(const PdnExtractSummary& s, const string& path) {
  ofstream f(path);
  if (!f)
    throw runtime_error("pdn_extract: cannot write report: " + path);
  f << "kind\tpower_net\tlayer\twire_count\th_tracks\tv_tracks\tpitch_um\twidth_um\tlink_a\tlink_b\tcomponents\n";
  for (const auto& l : s.layers) {
    f << "layer\t" << s.powerNet << '\t' << l.name << '\t' << l.wireCount << '\t' << l.hTrackCount << '\t'
      << l.vTrackCount << '\t' << l.pitchUm << '\t' << l.widthUm << "\t\t\t" << s.connectedComponents << '\n';
  }
  for (const auto& e : s.viaLinks) {
    f << "via\t" << s.powerNet << "\t\t\t\t\t\t\t" << e.first << '\t' << e.second << '\t'
      << s.connectedComponents << '\n';
  }
}

}  // namespace phys
