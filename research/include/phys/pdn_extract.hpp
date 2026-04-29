#ifndef PHYS_PDN_EXTRACT_HPP
#define PHYS_PDN_EXTRACT_HPP

#include <phys/chip.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace phys {

struct PdnExtractLayer {
  string name;
  int wireCount{};
  int hTrackCount{};
  int vTrackCount{};
  double pitchUm{};
  double widthUm{};
};

struct PdnExtractSummary {
  string powerNet;
  vector<PdnExtractLayer> layers;
  vector<pair<string, string>> viaLinks;
  int connectedComponents{};
};

PdnExtractSummary buildPdnExtractSummary(const Chip& chip);
void writePdnExtractSummaryTsv(const PdnExtractSummary& summary, const string& path);

}  // namespace phys

#endif  // PHYS_PDN_EXTRACT_HPP
