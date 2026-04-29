#ifndef PHYS_PDN_MODEL_HPP
#define PHYS_PDN_MODEL_HPP

#include <phys/chip.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace phys {

/// PDN geometry/connectivity only (no current loads).
struct PdnModel {
  struct Layer {
    string name;
    bool isHoriz{};
    double pitchUm{};
    double widthUm{};
    double gSeg{};
    int rank{};
  };
  vector<Layer> layers;
  vector<pair<int, int>> viaPairs;
};

/// Build PDN geometry/connectivity. Default: `add_pdn_stripe` / `add_pdn_connect` from `pdn_tcl`.
/// Set `PHYS_PDN_PARSE_SOURCE=odb` and populate `pdn_shapes_file` / `pdn_vias_file` for DB shapes.
PdnModel buildPdnModel(const Chip& chip);

/// Alias for `buildPdnModel` (historical name). DB-style geometry: set `PHYS_PDN_PARSE_SOURCE=odb`.
PdnModel buildPdnModelPdnsimStyle(const Chip& chip);

}  // namespace phys

#endif  // PHYS_PDN_MODEL_HPP
