#!/usr/bin/env tclsh
# SPDX-License-Identifier: BSD-3-Clause
#
# Unified database extractor for research flow.
# Source-of-truth inputs:
#   - floorplan ODB (required)
#   - floorplan SDC (optional but recommended)
#
# Outputs under OUT_DIR:
#   db_layout.tsv
#   db_instances.tsv
#   db_pdn_shapes.tsv
#   db_sdc_summary.tsv
#   psm_vsrc_<NET>.loc
#   psm_vsrc_boxes_<NET>.tsv
#   pdn_vias_<NET>.tsv
#
# Usage:
#   openroad -exit -no_init -python <<'PY'  # or openroad -exit script.tcl with env vars
#   ...
#   PY
#
# Expected env vars:
#   ODB_FILE   : absolute/relative path to 2_floorplan.odb
#   OUT_DIR    : output directory
#   SCRIPTS_DIR: ORFS flow/scripts directory (for export helpers)
# Optional:
#   SDC_FILE   : path to 2_floorplan.sdc

proc req_env {k} {
  if {![info exists ::env($k)] || $::env($k) eq ""} {
    error "missing required env $k"
  }
  return $::env($k)
}

proc opt_env {k {d ""}} {
  if {![info exists ::env($k)] || $::env($k) eq ""} {
    return $d
  }
  return $::env($k)
}

# Compatibility helper used by ORFS export scripts.
proc env_var_exists_and_non_empty {k} {
  expr {[info exists ::env($k)] && $::env($k) ne ""}
}

proc fmt_um {dbu} {
  return [format "%.10g" [ord::dbu_to_microns $dbu]]
}

proc safe_layer_name {sbox} {
  set layer_name "?"
  if {![catch {set l [$sbox getTechLayer]}] && $l ne "" && $l ne "NULL"} {
    catch {set layer_name [$l getName]}
  }
  return $layer_name
}

proc extract_layout_tsv {out_dir} {
  set block [ord::get_db_block]
  set die [$block getDieArea]
  set core [$block getCoreArea]
  set p [file join $out_dir db_layout.tsv]
  set f [open $p w]
  puts $f "kind\tllx_um\tlly_um\turx_um\tury_um"
  puts $f "die\t[fmt_um [$die xMin]]\t[fmt_um [$die yMin]]\t[fmt_um [$die xMax]]\t[fmt_um [$die yMax]]"
  puts $f "core\t[fmt_um [$core xMin]]\t[fmt_um [$core yMin]]\t[fmt_um [$core xMax]]\t[fmt_um [$core yMax]]"
  close $f
  puts "extract: wrote $p"
}

proc extract_instances_tsv {out_dir} {
  set block [ord::get_db_block]
  set p [file join $out_dir db_instances.tsv]
  set f [open $p w]
  puts $f "instance\tmaster\tis_macro\tllx_um\tlly_um\turx_um\tury_um\tcx_um\tcy_um\tarea_um2\tpg_pin_name\tpg_pin_x_um\tpg_pin_y_um"
  set n 0
  set pg_pin_rows 0
  set pg_pin_bbox_fallback 0
  set macro_without_pg_pin 0
  foreach inst [$block getInsts] {
    set master [$inst getMaster]
    if {$master eq "" || $master eq "NULL"} {
      continue
    }
    set bbox [$inst getBBox]
    set llx [$bbox xMin]
    set lly [$bbox yMin]
    set urx [$bbox xMax]
    set ury [$bbox yMax]
    set cx [expr {($llx + $urx) / 2}]
    set cy [expr {($lly + $ury) / 2}]
    set w_um [ord::dbu_to_microns [expr {$urx - $llx}]]
    set h_um [ord::dbu_to_microns [expr {$ury - $lly}]]
    set area_um2 [expr {$w_um * $h_um}]
    set is_macro 0
    catch {set is_macro [expr {[$master isBlock] ? 1 : 0}]}
    set wrote_pin 0
    foreach iterm [$inst getITerms] {
      set mterm [$iterm getMTerm]
      if {$mterm eq "" || $mterm eq "NULL"} {
        continue
      }
      set sig [$mterm getSigType]
      if {$sig ne "POWER" && $sig ne "GROUND"} {
        continue
      }
      set ok 0
      set px 0
      set py 0
      if {![catch {set ok [$iterm getAvgXY px py]}]} {
        if {$ok} {
          puts $f "[$inst getName]\t[$master getName]\t$is_macro\t[fmt_um $llx]\t[fmt_um $lly]\t[fmt_um $urx]\t[fmt_um $ury]\t[fmt_um $cx]\t[fmt_um $cy]\t[format %.10g $area_um2]\t[$mterm getName]\t[fmt_um $px]\t[fmt_um $py]"
          set wrote_pin 1
          incr pg_pin_rows
          continue
        }
      }
      # Fallback for macros when getAvgXY is unavailable: use transformed ITerm bbox center.
      # This still comes from ODB pin geometry and is more robust on abstracted SRAM macros.
      if {$is_macro} {
        if {![catch {set ibox [$iterm getBBox]}]} {
          if {$ibox ne "" && $ibox ne "NULL"} {
            set ix0 [$ibox xMin]
            set iy0 [$ibox yMin]
            set ix1 [$ibox xMax]
            set iy1 [$ibox yMax]
            if {$ix1 > $ix0 && $iy1 > $iy0} {
              set pbx [expr {($ix0 + $ix1) / 2}]
              set pby [expr {($iy0 + $iy1) / 2}]
              puts $f "[$inst getName]\t[$master getName]\t$is_macro\t[fmt_um $llx]\t[fmt_um $lly]\t[fmt_um $urx]\t[fmt_um $ury]\t[fmt_um $cx]\t[fmt_um $cy]\t[format %.10g $area_um2]\t[$mterm getName]\t[fmt_um $pbx]\t[fmt_um $pby]"
              set wrote_pin 1
              incr pg_pin_rows
              incr pg_pin_bbox_fallback
            }
          }
        }
      }
    }
    if {!$wrote_pin} {
      puts $f "[$inst getName]\t[$master getName]\t$is_macro\t[fmt_um $llx]\t[fmt_um $lly]\t[fmt_um $urx]\t[fmt_um $ury]\t[fmt_um $cx]\t[fmt_um $cy]\t[format %.10g $area_um2]\t\t\t"
      if {$is_macro} {
        incr macro_without_pg_pin
      }
    }
    incr n
  }
  close $f
  puts "extract: wrote $p ($n rows, pg_pin_rows=$pg_pin_rows, bbox_fallback=$pg_pin_bbox_fallback, macro_without_pg_pin=$macro_without_pg_pin)"
  return [list $n $pg_pin_rows $pg_pin_bbox_fallback $macro_without_pg_pin]
}

proc extract_pdn_shapes_tsv {out_dir} {
  set block [ord::get_db_block]
  set p [file join $out_dir db_pdn_shapes.tsv]
  set f [open $p w]
  puts $f "net\tsig_type\tshape_kind\tlayer\tllx_um\tlly_um\turx_um\tury_um\tcx_um\tcy_um"
  set n 0
  foreach net [$block getNets] {
    set sig [$net getSigType]
    if {$sig ne "POWER" && $sig ne "GROUND"} {
      continue
    }
    set net_name [$net getName]
    foreach swire [$net getSWires] {
      foreach sbox [$swire getWires] {
        set llx [$sbox xMin]
        set lly [$sbox yMin]
        set urx [$sbox xMax]
        set ury [$sbox yMax]
        set cx [expr {($llx + $urx) / 2}]
        set cy [expr {($lly + $ury) / 2}]
        if {[$sbox isVia]} {
          set kind "via"
          set layer "?"
        } else {
          set kind "wire"
          set layer [safe_layer_name $sbox]
        }
        puts $f "$net_name\t$sig\t$kind\t$layer\t[fmt_um $llx]\t[fmt_um $lly]\t[fmt_um $urx]\t[fmt_um $ury]\t[fmt_um $cx]\t[fmt_um $cy]"
        incr n
      }
    }
  }
  close $f
  puts "extract: wrote $p ($n rows)"
}

proc extract_sdc_summary_tsv {sdc_file out_dir} {
  if {$sdc_file eq "" || ![file exists $sdc_file]} {
    puts "extract: no SDC_FILE provided; skip SDC summary"
    return
  }
  set in [open $sdc_file r]
  set out_path [file join $out_dir db_sdc_summary.tsv]
  set out [open $out_path w]
  puts $out "kind\tname\tvalue\tobject"
  while {[gets $in line] >= 0} {
    set t [string trim $line]
    if {$t eq "" || [string index $t 0] eq "#"} {
      continue
    }
    if {[regexp {^current_design\s+(\S+)} $t -> d]} {
      puts $out "design\t$d\t\t"
    } elseif {[regexp {^create_clock\s+-name\s+(\S+)\s+-period\s+(\S+)\s+(.+)$} $t -> n per obj]} {
      puts $out "clock\t$n\t$per\t$obj"
    } elseif {[regexp {^set_clock_uncertainty\s+(\S+)\s+(\S+)} $t -> v clk]} {
      puts $out "clock_uncertainty\t$clk\t$v\t"
    } elseif {[regexp {^set_clock_latency\s+(\S+)\s+(.+)$} $t -> v obj]} {
      puts $out "clock_latency\t\t$v\t$obj"
    } elseif {[regexp {^set_input_delay\s+(\S+)\s+(.+)$} $t -> v obj]} {
      puts $out "input_delay\t\t$v\t$obj"
    } elseif {[regexp {^set_output_delay\s+(\S+)\s+(.+)$} $t -> v obj]} {
      puts $out "output_delay\t\t$v\t$obj"
    } elseif {[regexp {^set_case_analysis\s+(\S+)\s+(.+)$} $t -> v obj]} {
      puts $out "case_analysis\t\t$v\t$obj"
    } elseif {[regexp {^set_max_transition\s+(\S+)\s+(.+)$} $t -> v obj]} {
      puts $out "max_transition\t\t$v\t$obj"
    } elseif {[regexp {^set_max_fanout\s+(\S+)\s+(.+)$} $t -> v obj]} {
      puts $out "max_fanout\t\t$v\t$obj"
    }
  }
  close $out
  close $in
  puts "extract: wrote $out_path"
}

set odb_file [req_env ODB_FILE]
set out_dir [req_env OUT_DIR]
set scripts_dir [req_env SCRIPTS_DIR]
set sdc_file [opt_env SDC_FILE ""]

file mkdir $out_dir
read_db $odb_file
if {$sdc_file ne "" && [file exists $sdc_file]} {
  catch {read_sdc $sdc_file}
}

source [file join $scripts_dir export_psm_vsrc.tcl]
source [file join $scripts_dir export_pdn_vias.tcl]

extract_layout_tsv $out_dir
set inst_stats [extract_instances_tsv $out_dir]
lassign $inst_stats inst_rows pg_pin_rows pg_pin_bbox_fallback macro_without_pg_pin

# Strict guardrail: fail if any hard macro still has no PG pin location.
# Override with ALLOW_MISSING_MACRO_PG_PINS=1 only for debugging.
set allow_missing_macro_pg 0
if {[env_var_exists_and_non_empty ALLOW_MISSING_MACRO_PG_PINS]} {
  set allow_missing_macro_pg [expr {$::env(ALLOW_MISSING_MACRO_PG_PINS) ? 1 : 0}]
}
if {!$allow_missing_macro_pg && $macro_without_pg_pin > 0} {
  error "extract: strict check failed: hard macros without PG pin locations = $macro_without_pg_pin (set ALLOW_MISSING_MACRO_PG_PINS=1 to bypass)"
}

extract_pdn_shapes_tsv $out_dir
extract_sdc_summary_tsv $sdc_file $out_dir
export_psm_vsrc_from_bterms $out_dir
export_pdn_vias_from_special_nets $out_dir

puts "extract: done (ODB=$odb_file, SDC=$sdc_file, OUT=$out_dir)"
