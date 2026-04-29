#!/usr/bin/env tclsh

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

proc extract_pdn_shapes_tsv {out_dir} {
  set block [ord::get_db_block]
  set p [file join $out_dir db_pdn_shapes.tsv]
  set f [open $p w]
  puts $f "net\tsig_type\tshape_kind\tlayer\tllx_um\tlly_um\turx_um\tury_um\tcx_um\tcy_um"
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
      }
    }
  }
  close $f
  puts "extract: wrote $p"
}

set odb_file [req_env ODB_FILE]
set out_dir [req_env OUT_DIR]
set sdc_file [opt_env SDC_FILE ""]
set scripts_dir [req_env SCRIPTS_DIR]

file mkdir $out_dir
read_db $odb_file
if {$sdc_file ne "" && [file exists $sdc_file]} {
  catch {read_sdc $sdc_file}
}

source [file join $scripts_dir export_pdn_vias.tcl]
source [file join $scripts_dir export_psm_vsrc.tcl]

extract_pdn_shapes_tsv $out_dir
export_pdn_vias_from_special_nets $out_dir
export_psm_vsrc_from_bterms $out_dir

puts "extract: done"
