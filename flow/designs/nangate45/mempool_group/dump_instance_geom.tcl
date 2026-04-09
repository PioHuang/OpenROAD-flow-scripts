read_db ./results/nangate45/mempool_group/base/2_2_floorplan_macro.odb

set out_tsv ./reports/nangate45/mempool_group/base/instance_geom.tsv
file mkdir [file dirname $out_tsv]

set block [ord::get_db_block]
set tech [ord::get_db_tech]
set dbu [$tech getDbUnitsPerMicron]

set f [open $out_tsv w]
puts $f "instance\tis_macro\tarea_um2\tcx_um\tcy_um\tpg_pin_x_um\tpg_pin_y_um"

foreach inst [$block getInsts] {
  set name [$inst getName]
  set is_macro [expr {[$inst isBlock] ? 1 : 0}]
  set master [$inst getMaster]
  set w [$master getWidth]
  set h [$master getHeight]
  set area_um2 [expr {($w * $h) / (1.0 * $dbu * $dbu)}]

  set cx ""
  set cy ""
  if {[$inst isPlaced]} {
    set box [$inst getBBox]
    set cx [expr {([$box xMin] + [$box xMax]) / (2.0 * $dbu)}]
    set cy [expr {([$box yMin] + [$box yMax]) / (2.0 * $dbu)}]
  }

  # Average POWER/GROUND ITerm locations for hard macros only.
  set pgx ""
  set pgy ""
  if {$is_macro} {
    set pg_sum_x 0.0
    set pg_sum_y 0.0
    set pg_n 0
    foreach iterm [$inst getITerms] {
      set mterm [$iterm getMTerm]
      set sig [$mterm getSigType]
      if {$sig ne "POWER" && $sig ne "GROUND"} {
        continue
      }
      lassign [$iterm getAvgXY] ok x y
      if {!$ok} {
        continue
      }
      set pg_sum_x [expr {$pg_sum_x + $x}]
      set pg_sum_y [expr {$pg_sum_y + $y}]
      incr pg_n
    }
    if {$pg_n > 0} {
      set pgx [expr {($pg_sum_x / $pg_n) / $dbu}]
      set pgy [expr {($pg_sum_y / $pg_n) / $dbu}]
    }
  }

  puts $f "$name\t$is_macro\t$area_um2\t$cx\t$cy\t$pgx\t$pgy"
}

close $f
puts "Wrote $out_tsv"
exit
