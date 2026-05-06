# Dump PDN straps, rings, and followpins of the active block to a CSV.
#
# Usage (inside OpenROAD Tcl, after the block has been loaded):
#
#   openroad -no_init -exit -no_splash \
#     -tech_lef platforms/nangate45/lef/NangateOpenCellLibrary.tech.lef \
#     ...                                                             \
#     /dev/stdin <<< '
#       source flow/scripts/dump_pdn_mesh.tcl
#       dump_pdn_mesh ./results/nangate45/mempool_group/base/2_4_floorplan_pdn.odb \
#                     ./out/pdn_dump.csv
#     '
#
# Or, from a flow context that already has a block loaded:
#
#   source flow/scripts/dump_pdn_mesh.tcl
#   dump_pdn_mesh_block [ord::get_db_block] ./out/pdn_dump.csv
#
# Output columns (tab-separated, one header row):
#   kind  layer  xlo_um  ylo_um  xhi_um  yhi_um
#
# kind is one of STRIPE | RING | FOLLOWPIN | BTERM.  Followpins are included
# so the research-side strap-sheet model can tell power rails from core/ring
# straps, even though the coarse mesh will typically ignore them.  BTERMs are
# the block-level VDD pins (created by define_pdn_grid -pins) and serve as
# voltage-source locations when no explicit power ring exists.

proc dump_pdn_mesh_block {block out_csv} {
  if {$block eq "NULL" || $block eq ""} {
    error "dump_pdn_mesh_block: no dbBlock (pass \[ord::get_db_block\])."
  }
  set units [$block getDefUnits]
  if {$units <= 0} {
    error "dump_pdn_mesh_block: invalid DEF units: $units"
  }

  set fh [open $out_csv w]
  puts $fh "kind\tlayer\txlo_um\tylo_um\txhi_um\tyhi_um"

  set total 0
  foreach net [$block getNets] {
    set st [$net getSigType]
    if {$st ne "POWER" && $st ne "GROUND"} {
      continue
    }
    if {![$net isSpecial]} {
      continue
    }
    # Only dump POWER segments; GROUND mesh mirrors POWER for the paper's
    # floorplan-stage IR analysis.
    if {$st ne "POWER"} {
      continue
    }
    foreach swire [$net getSWires] {
      foreach wire [$swire getWires] {
        if {[$wire isVia]} {
          continue
        }
        set shape [$wire getWireShapeType]
        set kind "STRIPE"
        if {$shape eq "FOLLOWPIN"} {
          set kind "FOLLOWPIN"
        } elseif {$shape eq "RING"} {
          set kind "RING"
        }
        set layer [[$wire getTechLayer] getName]
        set xlo [expr {[$wire xMin] / double($units)}]
        set ylo [expr {[$wire yMin] / double($units)}]
        set xhi [expr {[$wire xMax] / double($units)}]
        set yhi [expr {[$wire yMax] / double($units)}]
        puts $fh "$kind\t$layer\t$xlo\t$ylo\t$xhi\t$yhi"
        incr total
      }
    }
  }
  # Dump POWER block terminals (BTERMs).  These are the top-level VDD pins
  # placed by define_pdn_grid -pins and mark where external supply enters the
  # block.  Used as voltage-source nodes when no power ring is present.
  foreach bterm [$block getBTerms] {
    set net [$bterm getNet]
    if {[$net getSigType] ne "POWER"} {
      continue
    }
    foreach bpin [$bterm getBPins] {
      foreach box [$bpin getBoxes] {
        set layer [[$box getTechLayer] getName]
        set xlo [expr {[$box xMin] / double($units)}]
        set ylo [expr {[$box yMin] / double($units)}]
        set xhi [expr {[$box xMax] / double($units)}]
        set yhi [expr {[$box yMax] / double($units)}]
        puts $fh "BTERM\t$layer\t$xlo\t$ylo\t$xhi\t$yhi"
        incr total
      }
    }
  }

  close $fh
  puts "dump_pdn_mesh: wrote $total segments to $out_csv"
}

proc dump_pdn_mesh {odb_path out_csv} {
  if {![file exists $odb_path]} {
    error "dump_pdn_mesh: odb not found: $odb_path"
  }
  read_db $odb_path
  set block [ord::get_db_block]
  dump_pdn_mesh_block $block $out_csv
}
