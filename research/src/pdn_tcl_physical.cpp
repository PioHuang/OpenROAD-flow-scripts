#include <phys/pdn_tcl_physical.hpp>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

namespace phys {
namespace {

int metalRankFromLayerName(const string& layer) {
  int rank = -1;
  int run = -1;
  for (char ch : layer) {
    if (isdigit(static_cast<unsigned char>(ch))) {
      if (run < 0)
        run = 0;
      run = run * 10 + (ch - '0');
      rank = run;
    } else {
      run = -1;
    }
  }
  return rank;
}

bool stripeDirectionHorizontal(const string& layer) {
  const int r = metalRankFromLayerName(layer);
  return r > 0 && (r % 2 == 1);
}

vector<double> stripeCenters1d(double off, double pitch, double half_w, double lo, double hi) {
  vector<double> out;
  if (pitch <= 0.0)
    return out;
  const int k_min = static_cast<int>(floor((lo - off + half_w) / pitch)) - 4;
  const int k_max = static_cast<int>(ceil((hi - off - half_w) / pitch)) + 4;
  for (int k = k_min; k <= k_max; ++k) {
    const double c = off + static_cast<double>(k) * pitch;
    if (c - half_w >= lo - 1e-9 && c + half_w <= hi + 1e-9)
      out.push_back(c);
  }
  return out;
}

const Rectangle& extentForStripes(const Layout& layout) {
  const Rectangle& die = layout.die();
  if (die.width() > 1e-9 && die.height() > 1e-9)
    return die;
  return layout.core();
}

}  // namespace

void write_pdn_tcl_physical_tsv(const Chip& chip, const fs::path& out_path) {
  const Rectangle& core = chip.layout().core();
  const Rectangle& extent = extentForStripes(chip.layout());

  ofstream out(out_path);
  if (!out)
    throw runtime_error("pdn_tcl_physical: cannot write: " + out_path.string());

  out << fixed << setprecision(6);
  out << "# pdn_tcl_physical: literal add_pdn_stripe rectangles (um) clipped to layout.core; "
         "add_pdn_connect has no via coordinates in Tcl.\n";
  out << "# core_llx\tcore_lly\tcore_urx\tcore_ury\n";
  out << "#\t" << core.llx() << '\t' << core.lly() << '\t' << core.urx() << '\t' << core.ury() << '\n';
  out << "# die_llx\tdie_lly\tdie_urx\tdie_ury\n";
  out << "#\t" << chip.layout().die().llx() << '\t' << chip.layout().die().lly() << '\t'
      << chip.layout().die().urx() << '\t' << chip.layout().die().ury() << '\n';
  out << "kind\tgrid\tlayer_lo\tlayer_hi\twidth_um\tpitch_um\toffset_um\tfollowpins\thoriz\tllx\tlly\turx\tury\n";

  const double elx = extent.llx(), ely = extent.lly(), eux = extent.urx(), euy = extent.ury();

  for (const auto& s : chip.tech().stripes()) {
    if (s.width <= 0.0 || s.pitch <= 0.0)
      continue;
    const double half = 0.5 * s.width;
    const bool horiz = stripeDirectionHorizontal(s.layer);
    if (horiz) {
      const auto centers = stripeCenters1d(s.offset, s.pitch, half, ely, euy);
      for (double yc : centers) {
        Rectangle raw(elx, yc - half, eux, yc + half);
        const auto clipped = intersect_rectangles(raw, core);
        if (!clipped)
          continue;
        out << "strap\t" << s.grid << '\t' << s.layer << "\t\t" << s.width << '\t' << s.pitch << '\t' << s.offset
            << '\t' << (s.followpins ? 1 : 0) << '\t' << 1 << '\t' << clipped->llx() << '\t' << clipped->lly()
            << '\t' << clipped->urx() << '\t' << clipped->ury() << '\n';
      }
    } else {
      const auto centers = stripeCenters1d(s.offset, s.pitch, half, elx, eux);
      for (double xc : centers) {
        Rectangle raw(xc - half, ely, xc + half, euy);
        const auto clipped = intersect_rectangles(raw, core);
        if (!clipped)
          continue;
        out << "strap\t" << s.grid << '\t' << s.layer << "\t\t" << s.width << '\t' << s.pitch << '\t' << s.offset
            << '\t' << (s.followpins ? 1 : 0) << '\t' << 0 << '\t' << clipped->llx() << '\t' << clipped->lly()
            << '\t' << clipped->urx() << '\t' << clipped->ury() << '\n';
      }
    }
  }

  for (const auto& c : chip.tech().pdnConnects()) {
    out << "connect\t" << c.grid << '\t' << c.layer_lower << '\t' << c.layer_upper
        << "\t\t\t\t\t\t\t\t\t\n";
  }
}

}  // namespace phys
