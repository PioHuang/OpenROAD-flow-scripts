#include <phys/chip.hpp>

#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace phys {
namespace {

using json = nlohmann::json;

std::filesystem::path resolve_repo_root(const std::filesystem::path& manifest_path,
                                        const json& root_json) {
  if (root_json.contains("repo_root") && !root_json["repo_root"].is_null()) {
    std::string s = root_json["repo_root"].get<std::string>();
    if (!s.empty())
      return std::filesystem::absolute(s);
  }
  std::filesystem::path p = manifest_path.parent_path();
  if (p.filename() == "research")
    return p.parent_path();
  return p;
}

void expect_file(const std::filesystem::path& p, const char* what) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec))
    throw std::runtime_error(std::string(what) + ": not a file: " + p.string());
}

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot read: " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<LayerRc> parse_set_rc_tcl(std::string_view text) {
  static const std::regex re(
      R"(set_layer_rc\s+-layer\s+(\S+)\s+-resistance\s+(\S+)\s+-capacitance\s+(\S+))");
  std::vector<LayerRc> out;
  std::string t(text);
  for (std::sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it) {
    LayerRc row;
    row.layer = (*it)[1].str();
    row.r = std::stod((*it)[2].str());
    row.c = std::stod((*it)[3].str());
    out.push_back(std::move(row));
  }
  return out;
}

void parse_wire_rc_tcl(std::string_view text, std::string& sig_layer, std::string& clk_layer) {
  static const std::regex sig_re(R"(set_wire_rc\s+-signal\s+-layer\s+(\S+))");
  static const std::regex clk_re(R"(set_wire_rc\s+-clock\s+-layer\s+(\S+))");
  std::string t(text);
  std::smatch m;
  if (std::regex_search(t, m, sig_re))
    sig_layer = m[1].str();
  if (std::regex_search(t, m, clk_re))
    clk_layer = m[1].str();
}

std::string fold_tcl_continuations(std::string t) {
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

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string w;
  while (iss >> w)
    out.push_back(w);
  return out;
}

void parse_pdn_tcl(std::string_view text, Tech& tech) {
  std::string t = fold_tcl_continuations(std::string(text));

  static const std::regex re_global_connect(R"(\bglobal_connect\b)");
  tech.global_connect_called = std::regex_search(t, re_global_connect);

  static const std::regex re_agc(
      R"(add_global_connection\s+-net\s+\{([^}]+)\}\s+-inst_pattern\s+\{([^}]*)\}\s+-pin_pattern\s+\{([^}]+)\}(?:\s+(-power|-ground))?)");
  for (std::sregex_iterator it(t.begin(), t.end(), re_agc), end; it != end; ++it) {
    PgConn row;
    row.net = (*it)[1].str();
    row.inst_pattern = (*it)[2].str();
    row.pin_pattern = (*it)[3].str();
    std::string tail = (*it)[4].str();
    if (tail == "-power")
      row.rail = "power";
    else if (tail == "-ground")
      row.rail = "ground";
    tech.pg_conn.push_back(std::move(row));
  }

  static const std::regex re_svd(
      R"(set_voltage_domain\s+-name\s+\{([^}]+)\}\s+-power\s+\{([^}]+)\}\s+-ground\s+\{([^}]+)\})");
  for (std::sregex_iterator it(t.begin(), t.end(), re_svd), end; it != end; ++it) {
    VDomain vd;
    vd.name = (*it)[1].str();
    vd.power_net = (*it)[2].str();
    vd.gnd_net = (*it)[3].str();
    tech.vdomains.push_back(std::move(vd));
  }

  static const std::regex re_dpg(
      R"(define_pdn_grid\s+-name\s+\{([^}]+)\}(?:\s+-voltage_domains\s+\{([^}]*)\})?(?:\s+-pins\s+\{([^}]*)\})?(?:\s+-macro)?(?:\s+-orient\s+\{([^}]*)\})?(?:\s+-halo\s+\{([^}]*)\})?(?:\s+-default)?)");
  for (std::sregex_iterator it(t.begin(), t.end(), re_dpg), end; it != end; ++it) {
    const std::smatch& m = *it;
    PdnGrid g;
    g.name = m[1].str();
    g.voltage_domains = split_ws(m[2].str());
    g.pins = split_ws(m[3].str());
    g.macro = (m[0].str().find("-macro") != std::string::npos);
    g.orient = m[4].str();
    std::vector<std::string> halo_toks = split_ws(m[5].str());
    if (halo_toks.size() >= 4) {
      g.has_halo = true;
      for (int i = 0; i < 4; ++i)
        g.halo[i] = std::stod(halo_toks[static_cast<size_t>(i)]);
    }
    g.is_default = (m[0].str().find("-default") != std::string::npos);
    tech.pdn_grids.push_back(std::move(g));
  }

  static const std::regex re_apc(
      R"(add_pdn_connect\s+-grid\s+\{([^}]+)\}\s+-layers\s+\{([^}]+)\})");
  for (std::sregex_iterator it(t.begin(), t.end(), re_apc), end; it != end; ++it) {
    std::vector<std::string> layers = split_ws((*it)[2].str());
    if (layers.size() < 2)
      continue;
    PdnConnect c;
    c.grid = (*it)[1].str();
    c.layer_lower = layers[0];
    c.layer_upper = layers[1];
    tech.pdn_connects.push_back(std::move(c));
  }

  static const std::regex re_aps(
      R"(add_pdn_stripe\s+-grid\s+\{([^}]*)\}\s+-layer\s+\{([^}]*)\}\s+-width\s+\{([^}]*)\}\s+-pitch\s+\{([^}]*)\}\s+-offset\s+\{([^}]*)\}(?:\s+-followpins)?)");
  for (std::sregex_iterator it(t.begin(), t.end(), re_aps), end; it != end; ++it) {
    Stripe s;
    s.grid = (*it)[1].str();
    s.layer = (*it)[2].str();
    s.width = std::stod((*it)[3].str());
    s.pitch = std::stod((*it)[4].str());
    s.offset = std::stod((*it)[5].str());
    s.followpins = ((*it)[0].str().find("-followpins") != std::string::npos);
    tech.stripes.push_back(std::move(s));
  }
}

std::vector<TrackRule> parse_tracks_tcl(std::string_view text) {
  static const std::regex re(
      R"(make_tracks\s+(\S+)\s+-x_offset\s+(\S+)\s+-x_pitch\s+(\S+)\s+-y_offset\s+(\S+)\s+-y_pitch\s+(\S+))");
  std::vector<TrackRule> out;
  std::string t(text);
  for (std::sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it) {
    TrackRule tr;
    tr.layer = (*it)[1].str();
    tr.x0 = std::stod((*it)[2].str());
    tr.xp = std::stod((*it)[3].str());
    tr.y0 = std::stod((*it)[4].str());
    tr.yp = std::stod((*it)[5].str());
    out.push_back(std::move(tr));
  }
  return out;
}

std::string trim_str(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  if (a == s.size())
    return {};
  size_t b = s.size() - 1;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b])))
    --b;
  return s.substr(a, b - a + 1);
}

std::vector<std::string> split_tsv_line(const std::string& line) {
  std::vector<std::string> out;
  out.reserve(8);
  size_t p = 0;
  while (true) {
    size_t q = line.find('\t', p);
    if (q == std::string::npos) {
      out.push_back(line.substr(p));
      break;
    }
    out.push_back(line.substr(p, q - p));
    p = q + 1;
  }
  return out;
}

double parse_double_relaxed(const std::string& s, bool* ok = nullptr) {
  std::string t = trim_str(s);
  if (t.empty()) {
    if (ok)
      *ok = false;
    return 0.0;
  }
  try {
    size_t idx = 0;
    double v = std::stod(t, &idx);
    // allow trailing spaces only
    while (idx < t.size() && std::isspace(static_cast<unsigned char>(t[idx])))
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

std::unordered_map<std::string, InstPower> parse_report_power_instances_tsv(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot read: " + path.string());

  std::string header;
  if (!std::getline(in, header))
    return {};
  auto cols = split_tsv_line(trim_str(header));
  std::unordered_map<std::string, size_t> idx;
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
    throw std::runtime_error(
        "report_power_instances.tsv missing required columns (need instance, internal_W, switching_W, leakage_W, total_W): "
        + path.string());
  }

  std::unordered_map<std::string, InstPower> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    auto f = split_tsv_line(line);
    if (static_cast<int>(f.size()) <= c_inst)
      continue;
    std::string inst = f[static_cast<size_t>(c_inst)];
    if (inst.empty())
      continue;

    auto get = [&](int c) -> std::string {
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
    out.emplace(std::move(inst), p);
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
  std::vector<MacroPgPin> pg_pins;
  bool has_pg_pin{};  // !pg_pins.empty()
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

std::unordered_map<std::string, InstGeom> parse_instance_geom_tsv(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot read: " + path.string());

  std::string header;
  if (!std::getline(in, header))
    return {};
  auto cols = split_tsv_line(trim_str(header));
  std::unordered_map<std::string, size_t> idx;
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
    throw std::runtime_error("instance_geom_tsv missing required column: instance");
  }

  std::unordered_map<std::string, InstGeom> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    auto f = split_tsv_line(line);
    if (static_cast<int>(f.size()) <= c_inst)
      continue;
    const std::string inst = f[static_cast<size_t>(c_inst)];
    if (inst.empty())
      continue;

    auto get = [&](int c) -> std::string {
      if (c < 0)
        return {};
      size_t cs = static_cast<size_t>(c);
      if (cs >= f.size())
        return {};
      return f[cs];
    };

    InstGeom g;
    if (c_is_macro >= 0) {
      const std::string v = get(c_is_macro);
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
      pin.name = (c_pgn >= 0) ? trim_str(get(c_pgn)) : std::string{};
      pin.x_um = px;
      pin.y_um = py;
      g.pg_pins.push_back(std::move(pin));
    }
    g.has_pg_pin = !g.pg_pins.empty();

    auto it = out.find(inst);
    if (it == out.end()) {
      out.emplace(inst, std::move(g));
    } else {
      merge_inst_geom(it->second, g);
    }
  }
  return out;
}

bool sep_line(const std::string& line) {
  if (line.size() < 20)
    return false;
  return line.find_first_not_of('-') == std::string::npos;
}

struct GroupInfo {
  int id{};
  std::string name;
};

std::unordered_map<std::string, GroupInfo> parse_simple_groups(std::istream& in) {
  std::unordered_map<std::string, GroupInfo> m;
  std::string line;
  while (std::getline(in, line)) {
    line = trim_str(std::move(line));
    if (line.empty() || line[0] == '#')
      continue;
    auto toks = split_ws(line);
    if (toks.size() < 2)
      continue;
    std::string inst = toks[0];
    int id{};
    try {
      id = std::stoi(toks[1]);
    } catch (...) {
      continue;
    }
    std::string gname = "cluster_" + std::to_string(id);
    if (toks.size() > 2) {
      // rtlmp_instance_to_cluster.txt: inst id cluster_name depth
      gname.clear();
      for (size_t i = 2; i + 1 < toks.size(); ++i) {
        if (!gname.empty())
          gname += " ";
        gname += toks[i];
      }
      if (gname.empty())
        gname = "cluster_" + std::to_string(id);
    }
    m[inst] = GroupInfo{id, std::move(gname)};
  }
  return m;
}

// flow/designs/.../rtlmp_extract.tcl dump: GROUP / DIRECT_INSTS / indented inst names
std::unordered_map<std::string, GroupInfo> parse_rtlmp_membership(std::istream& in) {
  std::unordered_map<std::string, int> group_name_to_id;
  std::unordered_map<std::string, GroupInfo> inst_to_group;
  int next_id = 0;
  int cur_id = -1;
  std::string cur_name;
  bool in_insts = false;
  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim_str(line);
    if (t.rfind("GROUP:", 0) == 0) {
      in_insts = false;
      cur_name = trim_str(t.substr(6));
      std::string gname = cur_name;
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
      std::string inst = trim_str(line);
      if (!inst.empty())
        inst_to_group[inst] = GroupInfo{cur_id, cur_name};
    }
  }
  return inst_to_group;
}

std::unordered_map<std::string, GroupInfo> load_groups_file(const std::filesystem::path& path) {
  std::string all = read_all(path);
  if (all.find("RTLMP cluster hierarchy") != std::string::npos) {
    std::istringstream ss(all);
    return parse_rtlmp_membership(ss);
  }
  std::istringstream ss(all);
  return parse_simple_groups(ss);
}

std::vector<FpBox> parse_rtl_fp_txt(std::string_view text, double dbu_per_um) {
  std::vector<FpBox> out;
  std::string t(text);
  std::istringstream in(t);
  std::string line;
  while (std::getline(in, line)) {
    line = trim_str(std::move(line));
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream ls(line);
    std::string name;
    long long x{}, y{}, w{}, h{};
    if (!(ls >> name >> x >> y >> w >> h))
      throw std::runtime_error("bad rtl_fp line: " + line);
    FpBox b;
    b.name = std::move(name);
    b.lx = x / dbu_per_um;
    b.ly = y / dbu_per_um;
    b.ux = (x + w) / dbu_per_um;
    b.uy = (y + h) / dbu_per_um;
    out.push_back(std::move(b));
  }
  return out;
}

void fill_rect(double* out4, const json& j) {
  if (!j.is_array() || j.size() != 4)
    throw std::runtime_error("layout rect must be array of 4 numbers");
  for (size_t i = 0; i < 4; ++i)
    out4[i] = j[i].get<double>();
}

void fill_halo(double* out2, const json& j) {
  if (!j.is_array() || j.size() != 2)
    throw std::runtime_error("halo must be array of 2 numbers");
  out2[0] = j[0].get<double>();
  out2[1] = j[1].get<double>();
}

}  // namespace

Chip load_chip(const std::filesystem::path& manifest_path) {
  std::ifstream mf(manifest_path);
  if (!mf)
    throw std::runtime_error("cannot open manifest: " + manifest_path.string());
  json j;
  mf >> j;

  const std::filesystem::path repo = resolve_repo_root(manifest_path, j);

  Chip chip;
  chip.name = j.at("name").get<std::string>();

  const json& ly = j.at("layout");
  fill_rect(chip.layout.die, ly.at("die"));
  fill_rect(chip.layout.core, ly.at("core"));
  fill_halo(chip.layout.halo, ly.at("halo"));
  if (ly.contains("odb") && !ly["odb"].is_null())
    chip.layout.odb = ly["odb"].get<std::string>();
  if (ly.contains("def") && !ly["def"].is_null())
    chip.layout.def = ly["def"].get<std::string>();

  auto resolve = [&](const std::string& rel) {
    return std::filesystem::weakly_canonical(repo / rel);
  };

  const json& kit = j.at("kit");
  chip.kit.tech_lef = resolve(kit.at("tech_lef").get<std::string>()).string();
  chip.kit.sc_lef = resolve(kit.at("sc_lef").get<std::string>()).string();
  for (const auto& e : kit.at("lef_extra"))
    chip.kit.lef_extra.push_back(resolve(e.get<std::string>()).string());
  for (const auto& e : kit.at("lib"))
    chip.kit.lib.push_back(resolve(e.get<std::string>()).string());

  expect_file(chip.kit.tech_lef, "tech_lef");
  expect_file(chip.kit.sc_lef, "sc_lef");
  for (const auto& p : chip.kit.lef_extra)
    expect_file(p, "lef_extra");
  for (const auto& p : chip.kit.lib)
    expect_file(p, "lib");

  const json& tc = j.at("tech");
  chip.tech.process_nm = tc.at("process_nm").get<int>();
  chip.tech.site = tc.at("site").get<std::string>();
  chip.tech.route_min = tc.at("route_min").get<std::string>();
  chip.tech.route_max = tc.at("route_max").get<std::string>();
  chip.tech.route_clk_min = tc.at("route_clk_min").get<std::string>();

  const std::filesystem::path set_rc = resolve(tc.at("set_rc_tcl").get<std::string>());
  const std::filesystem::path tracks_tcl = resolve(tc.at("tracks_tcl").get<std::string>());
  const std::filesystem::path pdn_tcl = resolve(tc.at("pdn_tcl").get<std::string>());
  expect_file(set_rc, "set_rc_tcl");
  expect_file(tracks_tcl, "tracks_tcl");
  expect_file(pdn_tcl, "pdn_tcl");

  std::string rc_text = read_all(set_rc);
  chip.tech.rc = parse_set_rc_tcl(rc_text);
  parse_wire_rc_tcl(rc_text, chip.tech.sig_layer, chip.tech.clk_layer);

  chip.tech.tracks = parse_tracks_tcl(read_all(tracks_tcl));
  parse_pdn_tcl(read_all(pdn_tcl), chip.tech);

  auto ensure_instance = [&](const std::string& name) -> Instance& {
    auto it = chip.instances.find(name);
    if (it == chip.instances.end()) {
      Instance inst;
      inst.name = name;
      it = chip.instances.emplace(name, std::move(inst)).first;
    }
    return it->second;
  };

  if (j.contains("pwr") && j["pwr"].is_array()) {
    for (const auto& e : j["pwr"]) {
      const std::string inst_name = e.at("inst").get<std::string>();
      const double w = e.at("w").get<double>();
      Instance& inst = ensure_instance(inst_name);
      inst.has_manual_power = true;
      inst.manual_power_W = w;
    }
  }

  // Optional: load OpenSTA per-instance power dump (TSV) produced by report_power.tcl.
  // Supported keys (relative to repo root):
  //   - report_power_instances_tsv
  //   - report_power_tsv
  //   - power_tsv
  {
    std::string key;
    if (j.contains("report_power_instances_tsv") && !j["report_power_instances_tsv"].is_null())
      key = "report_power_instances_tsv";
    else if (j.contains("report_power_tsv") && !j["report_power_tsv"].is_null())
      key = "report_power_tsv";
    else if (j.contains("power_tsv") && !j["power_tsv"].is_null())
      key = "power_tsv";

    if (!key.empty()) {
      std::string rel = j[key].get<std::string>();
      if (!rel.empty()) {
        std::filesystem::path pp = resolve(rel);
        expect_file(pp, key.c_str());
        auto sta_map = parse_report_power_instances_tsv(pp);
        for (auto& kv : sta_map) {
          Instance& inst = ensure_instance(kv.first);
          inst.has_sta_power = true;
          inst.sta_power = kv.second;
        }
      }
    }
  }

  // Optional: geometry extracted from OpenROAD/ODB.
  // Expected TSV columns:
  //   instance (required),
  //   area_um2, cx_um, cy_um, pg_pin_name (optional), pg_pin_x_um, pg_pin_y_um (optional).
  //   Multiple rows with the same instance append PG pins (macros with several POWER/GROUND iterms).
  // Supported keys:
  //   - instance_geom_tsv
  //   - instance_geometry_tsv
  {
    std::string key;
    if (j.contains("instance_geom_tsv") && !j["instance_geom_tsv"].is_null())
      key = "instance_geom_tsv";
    else if (j.contains("instance_geometry_tsv") && !j["instance_geometry_tsv"].is_null())
      key = "instance_geometry_tsv";

    if (!key.empty()) {
      std::string rel = j[key].get<std::string>();
      if (!rel.empty()) {
        std::filesystem::path gp = resolve(rel);
        expect_file(gp, key.c_str());
        auto geom = parse_instance_geom_tsv(gp);
        for (const auto& kv : geom) {
          Instance& inst = ensure_instance(kv.first);
          const auto& g = kv.second;
          if (g.has_is_macro) {
            inst.is_macro = g.is_macro;
          }
          if (g.has_area) {
            inst.has_area = true;
            inst.area_um2 = g.area_um2;
          }
          if (g.has_loc) {
            inst.has_loc = true;
            inst.cx_um = g.cx_um;
            inst.cy_um = g.cy_um;
          }
          inst.pg_pins = g.pg_pins;
          inst.has_pg_pin = g.has_pg_pin;
        }
      }
    }
  }

  if (j.contains("groups") && !j["groups"].is_null()) {
    std::string gpath = j["groups"].get<std::string>();
    if (!gpath.empty()) {
      std::filesystem::path gp = resolve(gpath);
      expect_file(gp, "groups");
      auto groups = load_groups_file(gp);
      for (const auto& kv : groups) {
        Instance& inst = ensure_instance(kv.first);
        inst.has_cluster = true;
        inst.cluster_id = kv.second.id;
        inst.cluster_name = kv.second.name;
      }
    }
  }

  if (j.contains("rtl_fp") && !j["rtl_fp"].is_null()) {
    std::string fp_path = j["rtl_fp"].get<std::string>();
    if (!fp_path.empty()) {
      std::filesystem::path fp = resolve(fp_path);
      expect_file(fp, "rtl_fp");
      double dbu = j.value("fp_dbu_per_um", 2000.0);
      chip.fp = parse_rtl_fp_txt(read_all(fp), dbu);
    }
  }

  if (!chip.layout.odb.empty()) {
    std::filesystem::path op = resolve(chip.layout.odb);
    expect_file(op, "layout.odb");
    chip.layout.odb = op.string();
  }
  if (!chip.layout.def.empty()) {
    std::filesystem::path dp = resolve(chip.layout.def);
    expect_file(dp, "layout.def");
    chip.layout.def = dp.string();
  }

  return chip;
}

}  // namespace phys
