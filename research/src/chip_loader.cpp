#include <phys/chip.hpp>

#include <nlohmann/json.hpp>
#include <string_view>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

using namespace std;
namespace fs = std::filesystem;

namespace phys {

namespace {

using json = nlohmann::json;

fs::path resolve_repo_root(const fs::path& manifest_path,
                                        const json& root_json) {
  if (root_json.contains("repo_root") && !root_json["repo_root"].is_null()) {
    string s = root_json["repo_root"].get<string>();
    if (!s.empty())
      return fs::absolute(s);
  }
  fs::path p = manifest_path.parent_path();
  if (p.filename() == "research")
    return p.parent_path();
  return p;
}

void expect_file(const fs::path& p, const char* what) {
  error_code ec;
  if (!fs::is_regular_file(p, ec))
    throw runtime_error(string(what) + ": not a file: " + p.string());
}

string read_all(const fs::path& path) {
  ifstream in(path);
  if (!in)
    throw runtime_error("cannot read: " + path.string());
  ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

vector<LayerRc> parse_set_rc_tcl(string_view text) {
  static const regex re(
      R"(set_layer_rc\s+-layer\s+(\S+)\s+-resistance\s+(\S+)\s+-capacitance\s+(\S+))");
  vector<LayerRc> out;
  string t(text);
  for (sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it) {
    LayerRc row;
    row.layer = (*it)[1].str();
    row.r = stod((*it)[2].str());
    row.c = stod((*it)[3].str());
    out.push_back(move(row));
  }
  return out;
}

void parse_wire_rc_tcl(string_view text, string& sig_layer, string& clk_layer) {
  static const regex sig_re(R"(set_wire_rc\s+-signal\s+-layer\s+(\S+))");
  static const regex clk_re(R"(set_wire_rc\s+-clock\s+-layer\s+(\S+))");
  string t(text);
  smatch m;
  if (regex_search(t, m, sig_re))
    sig_layer = m[1].str();
  if (regex_search(t, m, clk_re))
    clk_layer = m[1].str();
}

string fold_tcl_continuations(string t) {
  for (size_t p = 0; p < t.size();) {
    if (t[p] == '\\' && p + 1 < t.size() && t[p + 1] == '\n') {
      size_t q = p + 2;
      while (q < t.size() && (t[q] == ' ' || t[q] == '\t' || t[q] == '\r'))
        ++q;
      t.replace(p, q - p, " ");
      continue;
    }
    ++p;
  }
  return t;
}

vector<string> split_ws(const string& s) {
  vector<string> out;
  istringstream iss(s);
  string w;
  while (iss >> w)
    out.push_back(w);
  return out;
}

vector<TrackRule> parse_tracks_tcl(string_view text) {
  static const regex re(
      R"(make_tracks\s+(\S+)\s+-x_offset\s+(\S+)\s+-x_pitch\s+(\S+)\s+-y_offset\s+(\S+)\s+-y_pitch\s+(\S+))");
  vector<TrackRule> out;
  string t(text);
  for (sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it) {
    TrackRule tr;
    tr.layer = (*it)[1].str();
    tr.x0 = stod((*it)[2].str());
    tr.xp = stod((*it)[3].str());
    tr.y0 = stod((*it)[4].str());
    tr.yp = stod((*it)[5].str());
    out.push_back(move(tr));
  }
  return out;
}

string trim_str(string s) {
  size_t a = 0;
  while (a < s.size() && isspace(static_cast<unsigned char>(s[a])))
    ++a;
  if (a == s.size())
    return {};
  size_t b = s.size() - 1;
  while (b > a && isspace(static_cast<unsigned char>(s[b])))
    --b;
  return s.substr(a, b - a + 1);
}

vector<string> split_tsv_line(const string& line) {
  vector<string> out;
  out.reserve(8);
  size_t p = 0;
  while (true) {
    size_t q = line.find('\t', p);
    if (q == string::npos) {
      out.push_back(line.substr(p));
      break;
    }
    out.push_back(line.substr(p, q - p));
    p = q + 1;
  }
  return out;
}

double parse_double_relaxed(const string& s, bool* ok = nullptr) {
  string t = trim_str(s);
  if (t.empty()) {
    if (ok)
      *ok = false;
    return 0.0;
  }
  try {
    size_t idx = 0;
    double v = stod(t, &idx);
    while (idx < t.size() && isspace(static_cast<unsigned char>(t[idx])))
      ++idx;
    if (idx != t.size()) {
      if (ok)
        *ok = false;
      return 0.0;
    }
    if (ok)
      *ok = true;
    return v;
  } catch (...) {
    if (ok)
      *ok = false;
    return 0.0;
  }
}

unordered_map<string, InstPower> parse_report_power_instances_tsv(
    const fs::path& path) {
  ifstream in(path);
  if (!in)
    throw runtime_error("cannot read: " + path.string());

  string header;
  if (!getline(in, header))
    return {};
  auto cols = split_tsv_line(trim_str(header));
  unordered_map<string, size_t> idx;
  for (size_t i = 0; i < cols.size(); ++i)
    idx[cols[i]] = i;

  auto col = [&](const char* name) -> int {
    auto it = idx.find(name);
    return it == idx.end() ? -1 : static_cast<int>(it->second);
  };

  const int c_inst = col("instance");
  const int c_int = col("internal_W");
  const int c_sw = col("switching_W");
  const int c_lk = col("leakage_W");
  const int c_tot = col("total_W");
  const int c_cur = col("current_A");

  if (c_inst < 0 || c_int < 0 || c_sw < 0 || c_lk < 0 || c_tot < 0) {
    throw runtime_error(
        "report_power_instances.tsv missing required columns (need instance, internal_W, switching_W, leakage_W, total_W): "
        + path.string());
  }

  unordered_map<string, InstPower> out;
  string line;
  while (getline(in, line)) {
    if (line.empty())
      continue;
    auto f = split_tsv_line(line);
    if (static_cast<int>(f.size()) <= c_inst)
      continue;
    string inst = f[static_cast<size_t>(c_inst)];
    if (inst.empty())
      continue;

    auto get = [&](int c) -> string {
      if (c < 0)
        return {};
      size_t cs = static_cast<size_t>(c);
      if (cs >= f.size())
        return {};
      return f[cs];
    };

    InstPower p;
    p.internal_W = parse_double_relaxed(get(c_int));
    p.switching_W = parse_double_relaxed(get(c_sw));
    p.leakage_W = parse_double_relaxed(get(c_lk));
    p.total_W = parse_double_relaxed(get(c_tot));
    if (c_cur >= 0) {
      bool ok = false;
      p.current_A = parse_double_relaxed(get(c_cur), &ok);
      p.has_current = ok;
    }
    out.emplace(move(inst), p);
  }
  return out;
}

struct InstGeom {
  bool has_is_macro{};
  bool is_macro{};
  bool has_area{};
  double area_um2{};
  bool has_loc{};
  double cx_um{};
  double cy_um{};
  vector<MacroPgPin> pg_pins;
  bool has_pg_pin{};
};

static void merge_inst_geom(InstGeom& a, const InstGeom& b) {
  if (b.has_is_macro) {
    a.has_is_macro = true;
    a.is_macro = b.is_macro;
  }
  if (b.has_area) {
    a.has_area = true;
    a.area_um2 = b.area_um2;
  }
  if (b.has_loc) {
    a.has_loc = true;
    a.cx_um = b.cx_um;
    a.cy_um = b.cy_um;
  }
  a.pg_pins.insert(a.pg_pins.end(), b.pg_pins.begin(), b.pg_pins.end());
  a.has_pg_pin = !a.pg_pins.empty();
}

unordered_map<string, InstGeom> parse_instance_geom_tsv(
    const fs::path& path) {
  ifstream in(path);
  if (!in)
    throw runtime_error("cannot read: " + path.string());

  string header;
  if (!getline(in, header))
    return {};
  auto cols = split_tsv_line(trim_str(header));
  unordered_map<string, size_t> idx;
  for (size_t i = 0; i < cols.size(); ++i)
    idx[cols[i]] = i;

  auto col = [&](const char* name) -> int {
    auto it = idx.find(name);
    return it == idx.end() ? -1 : static_cast<int>(it->second);
  };
  const int c_inst = col("instance");
  const int c_is_macro = col("is_macro");
  const int c_area = col("area_um2");
  const int c_cx = col("cx_um");
  const int c_cy = col("cy_um");
  const int c_pgn = col("pg_pin_name");
  const int c_pgx = col("pg_pin_x_um");
  const int c_pgy = col("pg_pin_y_um");
  if (c_inst < 0) {
    throw runtime_error("instance_geom_tsv missing required column: instance");
  }

  unordered_map<string, InstGeom> out;
  string line;
  while (getline(in, line)) {
    if (line.empty())
      continue;
    auto f = split_tsv_line(line);
    if (static_cast<int>(f.size()) <= c_inst)
      continue;
    const string inst = f[static_cast<size_t>(c_inst)];
    if (inst.empty())
      continue;

    auto get = [&](int c) -> string {
      if (c < 0)
        return {};
      size_t cs = static_cast<size_t>(c);
      if (cs >= f.size())
        return {};
      return f[cs];
    };

    InstGeom g;
    if (c_is_macro >= 0) {
      const string v = get(c_is_macro);
      if (!v.empty()) {
        g.has_is_macro = true;
        g.is_macro = (v == "1" || v == "true" || v == "TRUE" || v == "True");
      }
    }
    bool ok = false;
    g.area_um2 = parse_double_relaxed(get(c_area), &ok);
    g.has_area = ok && g.area_um2 > 0.0;
    g.cx_um = parse_double_relaxed(get(c_cx), &ok);
    g.has_loc = ok;
    bool ok_y = false;
    g.cy_um = parse_double_relaxed(get(c_cy), &ok_y);
    g.has_loc = g.has_loc && ok_y;

    bool okx = false;
    bool oky = false;
    const double px = parse_double_relaxed(get(c_pgx), &okx);
    const double py = parse_double_relaxed(get(c_pgy), &oky);
    if (okx && oky) {
      MacroPgPin pin;
      pin.name = (c_pgn >= 0) ? trim_str(get(c_pgn)) : string{};
      pin.x_um = px;
      pin.y_um = py;
      g.pg_pins.push_back(move(pin));
    }
    g.has_pg_pin = !g.pg_pins.empty();

    auto it = out.find(inst);
    if (it == out.end()) {
      out.emplace(inst, move(g));
    } else {
      merge_inst_geom(it->second, g);
    }
  }
  return out;
}

bool sep_line(const string& line) {
  if (line.size() < 20)
    return false;
  return line.find_first_not_of('-') == string::npos;
}

struct GroupInfo {
  int id{};
  string name;
};

unordered_map<string, GroupInfo> parse_simple_groups(istream& in) {
  unordered_map<string, GroupInfo> m;
  string line;
  while (getline(in, line)) {
    line = trim_str(move(line));
    if (line.empty() || line[0] == '#')
      continue;
    auto toks = split_ws(line);
    if (toks.size() < 2)
      continue;
    string inst = toks[0];
    int id{};
    try {
      id = stoi(toks[1]);
    } catch (...) {
      continue;
    }
    string gname = "cluster_" + to_string(id);
    if (toks.size() > 2) {
      gname.clear();
      for (size_t i = 2; i + 1 < toks.size(); ++i) {
        if (!gname.empty())
          gname += " ";
        gname += toks[i];
      }
      if (gname.empty())
        gname = "cluster_" + to_string(id);
    }
    m[inst] = GroupInfo{id, move(gname)};
  }
  return m;
}

unordered_map<string, GroupInfo> parse_rtlmp_membership(istream& in) {
  unordered_map<string, int> group_name_to_id;
  unordered_map<string, GroupInfo> inst_to_group;
  int next_id = 0;
  int cur_id = -1;
  string cur_name;
  bool in_insts = false;
  string line;
  while (getline(in, line)) {
    string t = trim_str(line);
    if (t.rfind("GROUP:", 0) == 0) {
      in_insts = false;
      cur_name = trim_str(t.substr(6));
      string gname = cur_name;
      auto it = group_name_to_id.find(gname);
      if (it == group_name_to_id.end()) {
        cur_id = next_id++;
        group_name_to_id.emplace(gname, cur_id);
      } else
        cur_id = it->second;
      continue;
    }
    if (t == "DIRECT_INSTS:") {
      in_insts = true;
      continue;
    }
    if (sep_line(t)) {
      in_insts = false;
      continue;
    }
    if (in_insts && cur_id >= 0 && line.size() >= 2 && line[0] == ' ' && line[1] == ' ') {
      string inst = trim_str(line);
      if (!inst.empty())
        inst_to_group[inst] = GroupInfo{cur_id, cur_name};
    }
  }
  return inst_to_group;
}

unordered_map<string, GroupInfo> load_groups_file(const fs::path& path) {
  string all = read_all(path);
  if (all.find("RTLMP cluster hierarchy") != string::npos) {
    istringstream ss(all);
    return parse_rtlmp_membership(ss);
  }
  istringstream ss(all);
  return parse_simple_groups(ss);
}

vector<FpBox> parse_rtl_fp_txt(string_view text, double dbu_per_um) {
  vector<FpBox> out;
  string t(text);
  istringstream in(t);
  string line;
  while (getline(in, line)) {
    line = trim_str(move(line));
    if (line.empty() || line[0] == '#')
      continue;
    istringstream ls(line);
    string name;
    long long x{}, y{}, w{}, h{};
    if (!(ls >> name >> x >> y >> w >> h))
      throw runtime_error("bad rtl_fp line: " + line);
    const double llx = x / dbu_per_um;
    const double lly = y / dbu_per_um;
    const double urx = (x + w) / dbu_per_um;
    const double ury = (y + h) / dbu_per_um;
    out.emplace_back(move(name), Rectangle(llx, lly, urx, ury));
  }
  return out;
}

Rectangle rectangle_from_json(const json& j) {
  if (!j.is_array() || j.size() != 4)
    throw runtime_error("layout rect must be array of 4 numbers");
  return Rectangle(j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>());
}

pair<double, double> halo_pair_from_json(const json& j) {
  if (!j.is_array() || j.size() != 2)
    throw runtime_error("halo must be array of 2 numbers");
  return {j[0].get<double>(), j[1].get<double>()};
}

}  // namespace

namespace detail {

void ChipLoader::parsePdnScript(string_view text, Tech& tech) {
  string t = fold_tcl_continuations(string(text));

  static const regex re_global_connect(R"(\bglobal_connect\b)");
  tech._globalConnectCalled = regex_search(t, re_global_connect);

  static const regex re_agc(
      R"(add_global_connection\s+-net\s+\{([^}]+)\}\s+-inst_pattern\s+\{([^}]*)\}\s+-pin_pattern\s+\{([^}]+)\}(?:\s+(-power|-ground))?)");
  for (sregex_iterator it(t.begin(), t.end(), re_agc), end; it != end; ++it) {
    PgConn row;
    row.net = (*it)[1].str();
    row.inst_pattern = (*it)[2].str();
    row.pin_pattern = (*it)[3].str();
    string tail = (*it)[4].str();
    if (tail == "-power")
      row.rail = "power";
    else if (tail == "-ground")
      row.rail = "ground";
    tech._globalConnections.push_back(move(row));
  }

  static const regex re_svd(
      R"(set_voltage_domain\s+-name\s+\{([^}]+)\}\s+-power\s+\{([^}]+)\}\s+-ground\s+\{([^}]+)\})");
  for (sregex_iterator it(t.begin(), t.end(), re_svd), end; it != end; ++it) {
    VDomain vd;
    vd.name = (*it)[1].str();
    vd.power_net = (*it)[2].str();
    vd.gnd_net = (*it)[3].str();
    tech._voltageDomains.push_back(move(vd));
  }

  static const regex re_dpg(
      R"(define_pdn_grid\s+-name\s+\{([^}]+)\}(?:\s+-voltage_domains\s+\{([^}]*)\})?(?:\s+-pins\s+\{([^}]*)\})?(?:\s+-macro)?(?:\s+-orient\s+\{([^}]*)\})?(?:\s+-halo\s+\{([^}]*)\})?(?:\s+-default)?)");
  for (sregex_iterator it(t.begin(), t.end(), re_dpg), end; it != end; ++it) {
    const smatch& m = *it;
    PdnGrid g;
    g.name = m[1].str();
    g.voltage_domains = split_ws(m[2].str());
    g.pins = split_ws(m[3].str());
    g.macro = (m[0].str().find("-macro") != string::npos);
    g.orient = m[4].str();
    vector<string> halo_toks = split_ws(m[5].str());
    if (halo_toks.size() >= 4) {
      g.has_halo = true;
      for (int i = 0; i < 4; ++i)
        g.halo[i] = stod(halo_toks[static_cast<size_t>(i)]);
    }
    g.is_default = (m[0].str().find("-default") != string::npos);
    tech._pdnGrids.push_back(move(g));
  }

  static const regex re_apc(
      R"(add_pdn_connect\s+-grid\s+\{([^}]+)\}\s+-layers\s+\{([^}]+)\})");
  for (sregex_iterator it(t.begin(), t.end(), re_apc), end; it != end; ++it) {
    vector<string> layers = split_ws((*it)[2].str());
    if (layers.size() < 2)
      continue;
    PdnConnect c;
    c.grid = (*it)[1].str();
    c.layer_lower = layers[0];
    c.layer_upper = layers[1];
    tech._pdnConnects.push_back(move(c));
  }

  static const regex re_aps(
      R"(add_pdn_stripe\s+-grid\s+\{([^}]*)\}\s+-layer\s+\{([^}]*)\}\s+-width\s+\{([^}]*)\}\s+-pitch\s+\{([^}]*)\}\s+-offset\s+\{([^}]*)\}(?:\s+-followpins)?)");
  for (sregex_iterator it(t.begin(), t.end(), re_aps), end; it != end; ++it) {
    Stripe s;
    s.grid = (*it)[1].str();
    s.layer = (*it)[2].str();
    s.width = stod((*it)[3].str());
    s.pitch = stod((*it)[4].str());
    s.offset = stod((*it)[5].str());
    s.followpins = ((*it)[0].str().find("-followpins") != string::npos);
    tech._stripes.push_back(move(s));
  }
}

Chip ChipLoader::load(const fs::path& manifest_path) {
  ifstream mf(manifest_path);
  if (!mf)
    throw runtime_error("cannot open manifest: " + manifest_path.string());
  json j;
  mf >> j;

  const fs::path repo = resolve_repo_root(manifest_path, j);

  Chip chip;
  chip._repoRoot = repo;
  chip._designName = j.at("name").get<string>();
  if (j.contains("psm_vsrc_file") && !j["psm_vsrc_file"].is_null())
    chip._psmVsrcFile = j["psm_vsrc_file"].get<string>();
  if (j.contains("psm_vsrc_boxes_file") && !j["psm_vsrc_boxes_file"].is_null())
    chip._psmVsrcBoxesFile = j["psm_vsrc_boxes_file"].get<string>();
  if (j.contains("pdn_vias_file") && !j["pdn_vias_file"].is_null())
    chip._pdnViasFile = j["pdn_vias_file"].get<string>();
  if (j.contains("pdn_shapes_file") && !j["pdn_shapes_file"].is_null())
    chip._pdnShapesFile = j["pdn_shapes_file"].get<string>();
  if (j.contains("ir_vsrc_center_node_only") && j["ir_vsrc_center_node_only"].is_boolean())
    chip._irVsrcCenterNodeOnly = j["ir_vsrc_center_node_only"].get<bool>();

  const json& ly = j.at("layout");
  chip._layout._die = rectangle_from_json(ly.at("die"));
  chip._layout._core = rectangle_from_json(ly.at("core"));
  const auto halo = halo_pair_from_json(ly.at("halo"));
  chip._layout._haloHorizontalUm = halo.first;
  chip._layout._haloVerticalUm = halo.second;
  if (ly.contains("odb") && !ly["odb"].is_null())
    chip._layout._odbPath = ly["odb"].get<string>();
  if (ly.contains("def") && !ly["def"].is_null())
    chip._layout._defPath = ly["def"].get<string>();

  auto resolve = [&](const string& rel) {
    return fs::weakly_canonical(repo / rel);
  };

  if (!chip._pdnShapesFile.empty()) {
    fs::path pp = resolve(chip._pdnShapesFile);
    expect_file(pp, "pdn_shapes_file");
    chip._pdnShapesFile = pp.string();
  }

  const json& kit = j.at("kit");
  chip._kit._techLefPath = resolve(kit.at("tech_lef").get<string>()).string();
  chip._kit._stdCellLefPath = resolve(kit.at("sc_lef").get<string>()).string();
  for (const auto& e : kit.at("lef_extra"))
    chip._kit._extraLefPaths.push_back(resolve(e.get<string>()).string());
  for (const auto& e : kit.at("lib"))
    chip._kit._libertyPaths.push_back(resolve(e.get<string>()).string());

  expect_file(chip._kit._techLefPath, "tech_lef");
  expect_file(chip._kit._stdCellLefPath, "sc_lef");
  for (const auto& p : chip._kit._extraLefPaths)
    expect_file(p, "lef_extra");
  for (const auto& p : chip._kit._libertyPaths)
    expect_file(p, "lib");

  const json& tc = j.at("tech");
  chip._tech._processNm = tc.at("process_nm").get<int>();
  chip._tech._siteName = tc.at("site").get<string>();
  chip._tech._routeMinLayer = tc.at("route_min").get<string>();
  chip._tech._routeMaxLayer = tc.at("route_max").get<string>();
  chip._tech._routeClkMinLayer = tc.at("route_clk_min").get<string>();

  const fs::path set_rc = resolve(tc.at("set_rc_tcl").get<string>());
  const fs::path tracks_tcl = resolve(tc.at("tracks_tcl").get<string>());
  const fs::path pdn_tcl = resolve(tc.at("pdn_tcl").get<string>());
  expect_file(set_rc, "set_rc_tcl");
  expect_file(tracks_tcl, "tracks_tcl");
  expect_file(pdn_tcl, "pdn_tcl");

  string rc_text = read_all(set_rc);
  chip._tech._layerRc = parse_set_rc_tcl(rc_text);
  parse_wire_rc_tcl(rc_text, chip._tech._signalWireRcLayer, chip._tech._clockWireRcLayer);

  chip._tech._trackRules = parse_tracks_tcl(read_all(tracks_tcl));
  ChipLoader::parsePdnScript(read_all(pdn_tcl), chip._tech);

  if (j.contains("pwr") && j["pwr"].is_array()) {
    for (const auto& e : j["pwr"]) {
      const string inst_name = e.at("inst").get<string>();
      const double w = e.at("w").get<double>();
      Instance& inst = chip.instanceOrInsert(inst_name);
      inst._hasManualPower = true;
      inst._manualPowerW = w;
    }
  }

  {
    string key;
    if (j.contains("report_power_instances_tsv") && !j["report_power_instances_tsv"].is_null())
      key = "report_power_instances_tsv";
    else if (j.contains("report_power_tsv") && !j["report_power_tsv"].is_null())
      key = "report_power_tsv";
    else if (j.contains("power_tsv") && !j["power_tsv"].is_null())
      key = "power_tsv";

    if (!key.empty()) {
      string rel = j[key].get<string>();
      if (!rel.empty()) {
        fs::path pp = resolve(rel);
        expect_file(pp, key.c_str());
        auto sta_map = parse_report_power_instances_tsv(pp);
        for (auto& kv : sta_map) {
          Instance& inst = chip.instanceOrInsert(kv.first);
          inst._hasStaPower = true;
          inst._staPower = kv.second;
        }
      }
    }
  }

  {
    string key;
    if (j.contains("instance_geom_tsv") && !j["instance_geom_tsv"].is_null())
      key = "instance_geom_tsv";
    else if (j.contains("instance_geometry_tsv") && !j["instance_geometry_tsv"].is_null())
      key = "instance_geometry_tsv";

    if (!key.empty()) {
      string rel = j[key].get<string>();
      if (!rel.empty()) {
        fs::path gp = resolve(rel);
        expect_file(gp, key.c_str());
        auto geom = parse_instance_geom_tsv(gp);
        for (const auto& kv : geom) {
          Instance& inst = chip.instanceOrInsert(kv.first);
          const auto& g = kv.second;
          if (g.has_is_macro) {
            inst._isMacro = g.is_macro;
          }
          if (g.has_area) {
            inst._hasArea = true;
            inst._areaUm2 = g.area_um2;
          }
          if (g.has_loc) {
            inst._hasLocation = true;
            inst._centerXUm = g.cx_um;
            inst._centerYUm = g.cy_um;
          }
          inst._pgPins = g.pg_pins;
          inst._hasPgPins = g.has_pg_pin;
        }
      }
    }
  }

  if (j.contains("groups") && !j["groups"].is_null()) {
    string gpath = j["groups"].get<string>();
    if (!gpath.empty()) {
      fs::path gp = resolve(gpath);
      expect_file(gp, "groups");
      auto groups = load_groups_file(gp);
      for (const auto& kv : groups) {
        Instance& inst = chip.instanceOrInsert(kv.first);
        inst._hasCluster = true;
        inst._clusterId = kv.second.id;
        inst._clusterName = kv.second.name;
      }
    }
  }

  if (j.contains("rtl_fp") && !j["rtl_fp"].is_null()) {
    string fp_path = j["rtl_fp"].get<string>();
    if (!fp_path.empty()) {
      fs::path fp = resolve(fp_path);
      expect_file(fp, "rtl_fp");
      double dbu = j.value("fp_dbu_per_um", 2000.0);
      chip._floorplanRegions = parse_rtl_fp_txt(read_all(fp), dbu);
    }
  }

  if (!chip._layout._odbPath.empty()) {
    fs::path op = resolve(chip._layout._odbPath);
    expect_file(op, "layout.odb");
    chip._layout._odbPath = op.string();
  }
  if (!chip._layout._defPath.empty()) {
    fs::path dp = resolve(chip._layout._defPath);
    expect_file(dp, "layout.def");
    chip._layout._defPath = dp.string();
  }

  return chip;
}

}  // namespace detail

Chip load_chip(const fs::path& manifest_path) {
  return detail::ChipLoader::load(manifest_path);
}

}  // namespace phys
