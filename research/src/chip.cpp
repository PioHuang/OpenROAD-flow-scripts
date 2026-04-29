#include <phys/chip.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <utility>

using namespace std;

namespace phys {

Rectangle::Rectangle(double llx, double lly, double urx, double ury)
    : _llx(llx), _lly(lly), _urx(urx), _ury(ury) {}

double Rectangle::width() const {
  return _urx - _llx;
}

double Rectangle::height() const {
  return _ury - _lly;
}

double Rectangle::area() const {
  const double w = width();
  const double h = height();
  if (w <= 0.0 || h <= 0.0)
    return 0.0;
  return w * h;
}

optional<Rectangle> intersect_rectangles(const Rectangle& a, const Rectangle& b) {
  const double llx = max(a.llx(), b.llx());
  const double lly = max(a.lly(), b.lly());
  const double urx = min(a.urx(), b.urx());
  const double ury = min(a.ury(), b.ury());
  if (llx >= urx || lly >= ury)
    return nullopt;
  return Rectangle(llx, lly, urx, ury);
}

FpBox::FpBox(string region_name, const Rectangle& bounds)
    : Rectangle(bounds),
      _regionName(move(region_name)) {}

Instance::Instance(string instance_name) : _instanceName(move(instance_name)) {}

Instance& Chip::instanceOrInsert(const string& name) {
  auto it = _instances.find(name);
  if (it == _instances.end())
    it = _instances.emplace(name, Instance(name)).first;
  return it->second;
}

namespace {

string trim_str(string s) {
  while (!s.empty() && isspace(static_cast<unsigned char>(s.front())))
    s.erase(0, 1);
  while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
    s.pop_back();
  return s;
}

optional<string> infer_platform_from_tech_lef(const string& tech_lef_path) {
  static const regex re(R"((?:^|/)flow/platforms/([^/]+)/)");
  smatch m;
  if (regex_search(tech_lef_path, m, re))
    return m[1].str();
  return nullopt;
}

optional<double> parse_pwr_nets_voltages_first_V(const fs::path& config_mk) {
  ifstream in(config_mk);
  if (!in)
    return nullopt;
  string line;
  while (getline(in, line)) {
    const size_t hash = line.find('#');
    if (hash != string::npos)
      line = line.substr(0, hash);
    if (line.find("PWR_NETS_VOLTAGES") == string::npos)
      continue;

    string rhs;
    const size_t qeq = line.find("?=");
    const size_t ceq = line.find(":=");
    if (qeq != string::npos)
      rhs = line.substr(qeq + 2);
    else if (ceq != string::npos)
      rhs = line.substr(ceq + 2);
    else {
      const size_t eq = line.find('=');
      if (eq == string::npos)
        continue;
      rhs = line.substr(eq + 1);
    }
    rhs = trim_str(move(rhs));
    if (rhs.empty() || rhs.find('$') != string::npos)
      continue;

    istringstream tok(rhs);
    string net_name;
    double v{};
    if (tok >> net_name >> v)
      return v;
  }
  return nullopt;
}

}  // namespace

optional<double> flow_supply_voltage_V(const Chip& chip) {
  const auto plat = infer_platform_from_tech_lef(chip.kit().techLefPath());
  if (!plat.has_value())
    return nullopt;
  const fs::path root = chip.repoRoot();
  if (root.empty())
    return nullopt;
  const string design = chip.designName();
  const fs::path design_mk = root / "flow" / "designs" / *plat / design / "config.mk";
  const fs::path platform_mk = root / "flow" / "platforms" / *plat / "config.mk";
  if (auto v = parse_pwr_nets_voltages_first_V(design_mk))
    return v;
  return parse_pwr_nets_voltages_first_V(platform_mk);
}

}  // namespace phys
