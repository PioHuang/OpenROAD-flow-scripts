# SPDX-License-Identifier: BSD-3-Clause
# Export real PDN via locations from the loaded floorplan ODB (after `pdngen`).
#
# Writes one TSV per supply net:
#   pdn_vias_<NET>.tsv
# columns:
#   x_um  y_um  llx_um  lly_um  urx_um  ury_um  lower_layer  upper_layer  via_name  net
#
# Usage:
#   source $::env(SCRIPTS_DIR)/export_pdn_vias.tcl
#   export_pdn_vias_from_special_nets $::env(REPORTS_DIR)

proc export_pdn_vias__obj_null { obj } {
  expr { $obj == "" || $obj == "NULL" }
}

proc export_pdn_vias_for_net { net_name reports_dir } {
  set block [ord::get_db_block]
  set net [$block findNet $net_name]
  if { [export_pdn_vias__obj_null $net] } {
    puts "export_pdn_vias: net \"$net_name\" not found"
    return 0
  }

  set out_path [file join $reports_dir "pdn_vias_${net_name}.tsv"]
  set out [open $out_path w]
  puts $out "# x_um\ty_um\tllx_um\tlly_um\turx_um\tury_um\tlower_layer\tupper_layer\tvia_name\tnet"

  set n 0
  foreach swire [$net getSWires] {
    foreach sbox [$swire getWires] {
      if { ![$sbox isVia] } {
        continue
      }

      set llx [$sbox xMin]
      set lly [$sbox yMin]
      set urx [$sbox xMax]
      set ury [$sbox yMax]
      # ord::dbu_to_microns expects integer DBU coordinates.
      set cx [expr {($llx + $urx) / 2}]
      set cy [expr {($lly + $ury) / 2}]

      set lower "?"
      set upper "?"
      set via_name "?"

      set tv ""
      if { ![catch { set tv [$sbox getTechVia] }] && ![export_pdn_vias__obj_null $tv] } {
        catch { set via_name [$tv getName] }
        if { ![catch { set bl [$tv getBottomLayer] }] && ![export_pdn_vias__obj_null $bl] } {
          catch { set lower [$bl getName] }
        }
        if { ![catch { set tl [$tv getTopLayer] }] && ![export_pdn_vias__obj_null $tl] } {
          catch { set upper [$tl getName] }
        }
      } else {
        set bv ""
        if { ![catch { set bv [$sbox getBlockVia] }] && ![export_pdn_vias__obj_null $bv] } {
          catch { set via_name [$bv getName] }
          if { ![catch { set bl [$bv getBottomLayer] }] && ![export_pdn_vias__obj_null $bl] } {
            catch { set lower [$bl getName] }
          }
          if { ![catch { set tl [$bv getTopLayer] }] && ![export_pdn_vias__obj_null $tl] } {
            catch { set upper [$tl getName] }
          }
        }
      }

      puts $out [format "%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t%.10g\t%s\t%s\t%s\t%s" \
          [ord::dbu_to_microns $cx] [ord::dbu_to_microns $cy] \
          [ord::dbu_to_microns $llx] [ord::dbu_to_microns $lly] \
          [ord::dbu_to_microns $urx] [ord::dbu_to_microns $ury] \
          $lower $upper $via_name $net_name]
      incr n
    }
  }

  close $out
  puts "export_pdn_vias: wrote $n via site(s) for net \"$net_name\" -> $out_path"
  return $n
}

proc export_pdn_vias_from_special_nets { reports_dir } {
  set block [ord::get_db_block]
  set nets [list]
  if { [env_var_exists_and_non_empty PWR_NETS_VOLTAGES] } {
    dict for { pwr_net_name _v } $::env(PWR_NETS_VOLTAGES) {
      lappend nets $pwr_net_name
    }
  } else {
    foreach n [$block getNets] {
      set t [$n getSigType]
      if { $t == "POWER" } {
        lappend nets [$n getName]
      }
    }
  }
  set nets [lsort -unique $nets]
  if { [llength $nets] == 0 } {
    puts "export_pdn_vias: no POWER nets found"
    return
  }
  foreach n $nets {
    export_pdn_vias_for_net $n $reports_dir
  }
}
