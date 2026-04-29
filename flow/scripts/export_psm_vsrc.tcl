# SPDX-License-Identifier: BSD-3-Clause
# Export OpenROAD PSM `-vsrc` geometry from the **loaded ODB** (package / block
# terminals on each power net). Format matches `ir_solver.cpp`
# `generateSourceNodesFromSourceFile`: each line is `x_um, y_um, size_um, voltage_V`.
#
# Usage (after `read_db` / `load_design`):
#   source $::env(SCRIPTS_DIR)/export_psm_vsrc.tcl
#   export_psm_vsrc_from_bterms $::env(REPORTS_DIR)
#
# Or enable `EXPORT_PSM_VSRC=1` in the design/platform `config.mk` so `pdn.tcl`
# (floorplan / `2_4_floorplan_pdn`) writes `psm_vsrc_<NET>.loc` under `REPORTS_DIR`
# for each entry in `PWR_NETS_VOLTAGES`.
#
# If a net has **no** bterm (internal PG only), that net is skipped with a log line;
# downstream tools may fall back to synthetic PSM bumps.

proc export_psm_vsrc__bterm_null { bterm } {
  if { $bterm == "" || $bterm == "NULL" } {
    return 1
  }
  return 0
}

# One bterm -> one or more lines (one per BPIN box), microns.
# Writes:
#   - OpenROAD PSM `-vsrc` lines: x, y, size, voltage (same as ir_solver.cpp input).
#   - `psm_vsrc_boxes_<NET>.tsv`: true **ODB** geometry for each dbBox on the VDD bterm — same shapes
#     OpenROAD uses for the pin in DEF (microns). Extra columns: `layer` (dbBox getTechLayer),
#     `bterm` (DEF pin / block-terminal name). Research/phys_load reads the first five fields only.
proc export_psm_vsrc_for_net { net_name voltage_v out_path { default_size_um 5.0 } } {
  set block [ord::get_db_block]
  set bterm [$block findBTerm $net_name]
  if { [export_psm_vsrc__bterm_null $bterm] } {
    puts "export_psm_vsrc: no bterm for net \"$net_name\" — skip $out_path"
    return 0
  }

  set pin_name [$bterm getConstName]
  set boxes_path [file join [file dirname $out_path] "psm_vsrc_boxes_${net_name}.tsv"]
  set out [open $out_path w]
  set outb [open $boxes_path w]
  puts $outb "# llx_um\tlly_um\turx_um\tury_um\tvoltage_V\tlayer\tbterm — from OpenROAD ODB (dbBTerm/dbBPin/dbBox); µm"
  set n 0
  foreach bpin [$bterm getBPins] {
    foreach box [$bpin getBoxes] {
      set xlo [$box xMin]
      set ylo [$box yMin]
      set xhi [$box xMax]
      set yhi [$box yMax]
      set xlo_um [ord::dbu_to_microns $xlo]
      set ylo_um [ord::dbu_to_microns $ylo]
      set xhi_um [ord::dbu_to_microns $xhi]
      set yhi_um [ord::dbu_to_microns $yhi]
      set layer_name "?"
      if { ! [catch { set layer_obj [$box getTechLayer] }] } {
        if { [info exists layer_obj] && $layer_obj != "NULL" && $layer_obj != "" } {
          catch { set layer_name [$layer_obj getName] }
        }
      }
      puts $outb [format "%.10g\t%.10g\t%.10g\t%.10g\t%s\t%s\t%s" $xlo_um $ylo_um $xhi_um $yhi_um $voltage_v $layer_name $pin_name]
      set cx_dbu [expr {($xlo + $xhi) / 2}]
      set cy_dbu [expr {($ylo + $yhi) / 2}]
      set x_um [ord::dbu_to_microns $cx_dbu]
      set y_um [ord::dbu_to_microns $cy_dbu]
      set dx [expr { $xhi - $xlo }]
      set dy [expr { $yhi - $ylo }]
      if { $dx > $dy } {
        set size_dbu $dx
      } else {
        set size_dbu $dy
      }
      set size_um [ord::dbu_to_microns $size_dbu]
      if { $size_um < 0.05 } {
        set size_um $default_size_um
      }
      puts $out [format "%s, %s, %s, %s" $x_um $y_um $size_um $voltage_v]
      incr n
    }
  }
  close $outb
  close $out
  puts "export_psm_vsrc: wrote $n PSM -vsrc line(s) to $out_path and true boxes to $boxes_path"
  return $n
}

# Collect unique power-net names that have a block terminal (IO / package bumps).
proc export_psm_vsrc__power_nets_with_bterms { } {
  set block [ord::get_db_block]
  set seen [dict create]
  foreach bterm [$block getBTerms] {
    set net [$bterm getNet]
    if { $net == "NULL" || $net == "" } {
      continue
    }
    if { [$net getSigType] != "POWER" } {
      continue
    }
    set nname [$net getName]
    if { ![dict exists $seen $nname] } {
      dict set seen $nname 1
    }
  }
  return [dict keys $seen]
}

# Uses env PWR_NETS_VOLTAGES (dict: netName voltage ...) when set (platform config.mk).
# If unset (e.g. older flows), optional fallback: power nets inferred from BTerms and
# EXPORT_PSM_VSRC_DEFAULT_VOLTAGE (default 1.0 V).
proc export_psm_vsrc_from_bterms { reports_dir } {
  set default_v 1.0
  if { [env_var_exists_and_non_empty EXPORT_PSM_VSRC_DEFAULT_VOLTAGE] } {
    set default_v $::env(EXPORT_PSM_VSRC_DEFAULT_VOLTAGE)
  }

  if { [env_var_exists_and_non_empty PWR_NETS_VOLTAGES] } {
    dict for { pwr_net_name pwr_voltage } $::env(PWR_NETS_VOLTAGES) {
      set out_path [file join $reports_dir "psm_vsrc_${pwr_net_name}.loc"]
      export_psm_vsrc_for_net $pwr_net_name $pwr_voltage $out_path
    }
    return
  }

  set nets [export_psm_vsrc__power_nets_with_bterms]
  if { [llength $nets] == 0 } {
    puts "export_psm_vsrc: PWR_NETS_VOLTAGES unset and no POWER bterms — skip vsrc export"
    return
  }
  puts "export_psm_vsrc: PWR_NETS_VOLTAGES unset — using POWER nets with bterms: $nets (voltage $default_v V; set PWR_NETS_VOLTAGES or EXPORT_PSM_VSRC_DEFAULT_VOLTAGE to control)"
  foreach pwr_net_name $nets {
    set out_path [file join $reports_dir "psm_vsrc_${pwr_net_name}.loc"]
    export_psm_vsrc_for_net $pwr_net_name $default_v $out_path
  }
}
