#ifndef PHYS_PDN_TCL_PHYSICAL_HPP
#define PHYS_PDN_TCL_PHYSICAL_HPP

#include <phys/chip.hpp>

#include <filesystem>

namespace phys {

/// Writes `pdn_tcl_physical.tsv`: one row per literal `add_pdn_stripe` repetition (axis-aligned
/// rectangles in µm, clipped to manifest `layout.core`) plus `add_pdn_connect` rows (no via x,y
/// in Tcl). Matches the research Python `plot.py pdn_tcl` strap geometry (die extent for pitch
/// math, clip to core; odd metal rank → horizontal straps).
void write_pdn_tcl_physical_tsv(const Chip& chip, const std::filesystem::path& out_path);

}  // namespace phys

#endif
