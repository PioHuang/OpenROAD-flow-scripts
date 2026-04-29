#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

namespace {

struct LayerObs {
  int wireCount{};
  vector<double> xw;
  vector<double> yw;
  vector<double> hTracks;
  vector<double> vTracks;
};

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    cerr << "Usage: " << argv[0] << " <pdn_shapes.tsv> <pdn_vias.tsv> <power_net> <out_tsv>\n";
    return 1;
  }
  const string shapesPath = argv[1];
  const string viasPath = argv[2];
  const string powerNet = argv[3];
  const string outPath = argv[4];

  ifstream shapes(shapesPath);
  if (!shapes)
    throw runtime_error("cannot read shapes file: " + shapesPath);
  unordered_map<string, LayerObs> byLayer;
  string line;
  getline(shapes, line);
  while (getline(shapes, line)) {
    if (line.empty())
      continue;
    const vector<string> tok = splitTab(line);
    if (tok.size() < 10)
      continue;
    if (tok[2] != "wire" || tok[1] != "POWER" || tok[0] != powerNet)
      continue;
    const string& layer = tok[3];
    const double llx = stod(tok[4]), lly = stod(tok[5]), urx = stod(tok[6]), ury = stod(tok[7]);
    const double dx = max(0.0, urx - llx), dy = max(0.0, ury - lly);
    auto& obs = byLayer[layer];
    obs.wireCount++;
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

  struct LayerRow {
    string name;
    int wireCount{};
    int hTracks{};
    int vTracks{};
    double pitch{};
    double width{};
  };
  vector<LayerRow> layers;
  for (const auto& kv : byLayer) {
    const auto& o = kv.second;
    const bool isHoriz = o.hTracks.size() >= o.vTracks.size();
    layers.push_back({kv.first,
                      o.wireCount,
                      static_cast<int>(o.hTracks.size()),
                      static_cast<int>(o.vTracks.size()),
                      isHoriz ? pitchFrom(o.hTracks) : pitchFrom(o.vTracks),
                      isHoriz ? median(o.yw) : median(o.xw)});
  }
  sort(layers.begin(), layers.end(), [](const auto& a, const auto& b) { return a.pitch < b.pitch; });

  ifstream vias(viasPath);
  if (!vias)
    throw runtime_error("cannot read vias file: " + viasPath);
  set<pair<string, string>> viaLinks;
  getline(vias, line);
  while (getline(vias, line)) {
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
    viaLinks.emplace(a, b);
  }

  unordered_map<string, int> id;
  for (size_t i = 0; i < layers.size(); ++i)
    id[layers[i].name] = static_cast<int>(i);
  vector<int> parent(layers.size());
  for (size_t i = 0; i < layers.size(); ++i)
    parent[i] = static_cast<int>(i);
  auto root = [&](int x) {
    int r = x;
    while (parent[r] != r)
      r = parent[r];
    while (parent[x] != x) {
      const int n = parent[x];
      parent[x] = r;
      x = n;
    }
    return r;
  };
  auto unite = [&](int a, int b) {
    a = root(a);
    b = root(b);
    if (a != b)
      parent[b] = a;
  };
  for (const auto& e : viaLinks) {
    const auto ia = id.find(e.first);
    const auto ib = id.find(e.second);
    if (ia != id.end() && ib != id.end())
      unite(ia->second, ib->second);
  }
  set<int> comps;
  for (size_t i = 0; i < layers.size(); ++i)
    comps.insert(root(static_cast<int>(i)));

  fs::create_directories(fs::path(outPath).parent_path());
  ofstream out(outPath);
  if (!out)
    throw runtime_error("cannot write output: " + outPath);
  out << "kind\tpower_net\tlayer\twire_count\th_tracks\tv_tracks\tpitch_um\twidth_um\tvia_a\tvia_b\tcomponents\n";
  for (const auto& l : layers) {
    out << "layer\t" << powerNet << '\t' << l.name << '\t' << l.wireCount << '\t' << l.hTracks << '\t'
        << l.vTracks << '\t' << l.pitch << '\t' << l.width << "\t\t\t" << comps.size() << '\n';
  }
  for (const auto& e : viaLinks) {
    out << "via\t" << powerNet << "\t\t\t\t\t\t\t" << e.first << '\t' << e.second << '\t' << comps.size() << '\n';
  }

  cout << "Wrote " << outPath << " (layers=" << layers.size() << ", via_links=" << viaLinks.size()
       << ", components=" << comps.size() << ")\n";
  return 0;
}
