# SPDX-License-Identifier: BSD-3-Clause
# Export non-via special-wire segments (straps, follow-pins, etc.) from the loaded
# ODB after `pdngen`. Each dbSBox that is not a via becomes one axis-aligned row.
#
# Writes one TSV per supply net:
#   pdn_wires_<NET>.tsv
# columns:
#   llx_um  lly_um  urx_um  ury_um  layer  shape  direction  net
#
# Usage:
#   source $::env(SCRIPTS_DIR)/export_pdn_wires.tcl
#   export_pdn_wires_from_special_nets $::env(REPORTS_DIR)
#
# Enable with EXPORT_PDN_WIRES=1 in config.mk (see flow/scripts/pdn.tcl).

proc export_pdn_wires__obj_null { obj } {
  expr { $obj == "" || $obj == "NULL" }
}

proc export_pdn_wires__shape_name { sbox } {
  if { [catch { set wt [$sbox getWireShapeType] }] } {
    return "?"
  }
  return $wt
}

proc export_pdn_wires__dir_name { sbox } {
  if { [catch { set d [$sbox getDir] }] } {
    return "?"
  }
  return $d
}

proc export_pdn_wires_for_net { net_name reports_dir } {
  set block [ord::get_db_block]
  set net [$block findNet $net_name]
  if { [export_pdn_wires__obj_null $net] } {
    puts "export_pdn_wires: net \"$net_name\" not found"
    return 0
  }

  set out_path [file join $reports_dir "pdn_wires_${net_name}.tsv"]
  set out [open $out_path w]
  puts $out "# llx_um\tlly_um\turx_um\tury_um\tlayer\tshape\tdirection\tnet"

  set n 0
  foreach swire [$net getSWires] {
    foreach sbox [$swire getWires] {
      if { [$sbox isVia] } {
        continue
      }

      set llx [$sbox xMin]
      set lly [$sbox yMin]
      set urx [$sbox xMax]
      set ury [$sbox yMax]

      set layer_name "?"
      if { ! [catch { set tl [$sbox getTechLayer] }] } {
        if { ! [export_pdn_wires__obj_null $tl] } {
          catch { set layer_name [$tl getName] }
        }
      }

      set shape_name [export_pdn_wires__shape_name $sbox]
      set dir_name [export_pdn_wires__dir_name $sbox]

      puts $out [format "%.10g\t%.10g\t%.10g\t%.10g\t%s\t%s\t%s\t%s" \
          [ord::dbu_to_microns $llx] [ord::dbu_to_microns $lly] \
          [ord::dbu_to_microns $urx] [ord::dbu_to_microns $ury] \
          $layer_name $shape_name $dir_name $net_name]
      incr n
    }
  }

  close $out
  puts "export_pdn_wires: wrote $n wire box(es) for net \"$net_name\" -> $out_path"
  return $n
}

proc export_pdn_wires_from_special_nets { reports_dir } {
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
    puts "export_pdn_wires: no POWER nets found"
    return
  }
  foreach n $nets {
    export_pdn_wires_for_net $n $reports_dir
  }
}
