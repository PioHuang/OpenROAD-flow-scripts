read_liberty ./platforms/nangate45/lib/NangateOpenCellLibrary_typical.lib
read_liberty ./platforms/nangate45/lib/fakeram45_256x32.lib
read_liberty ./platforms/nangate45/lib/fakeram45_64x64.lib

# --- Mode --------------------------------------------------------------------
# 1 (default): Load make do-2_2_floorplan_macro output and dump reports only.
#               Matches clustering/macros in 2_2_floorplan_macro.odb (requires
#               RTLMP_KEEP_CLUSTERING_DATA=1 in config.mk for VISUAL_DEBUG groups).
# 0: Legacy — load 2_1, rerun rtl_macro_placer, then dump (may differ from flow 2_2).
set RTLMP_EXTRACT_DUMP_FROM_2_2 1

set results_dir ./results/nangate45/mempool_group/base

if { $RTLMP_EXTRACT_DUMP_FROM_2_2 } {
  read_db $results_dir/2_2_floorplan_macro.odb
  read_sdc $results_dir/2_1_floorplan.sdc
} else {
  read_db $results_dir/2_1_floorplan.odb
  read_sdc $results_dir/2_1_floorplan.sdc
}

source ./platforms/nangate45/setRC.tcl

file mkdir ./objects/nangate45/mempool_group/base/rtlmp_extract
file mkdir ./reports/nangate45/mempool_group/base

if { !$RTLMP_EXTRACT_DUMP_FROM_2_2 } {
  # Enable RTLMP debug floorplan dumps (.fp.txt / .cost.txt / .net.txt).
  set_debug_level MPL hierarchical_macro_placement 1

  rtl_macro_placer \
    -max_num_level 1 \
    -halo_width 10 \
    -halo_height 10 \
    -min_ar 0.33 \
    -area_weight 0.1 \
    -wirelength_weight 100.0 \
    -outline_weight 100.0 \
    -boundary_weight 50.0 \
    -notch_weight 50.0 \
    -report_directory ./objects/nangate45/mempool_group/base/rtlmp_extract \
    -fence_lx 0.0 \
    -fence_ly 0.0 \
    -fence_ux 0.0 \
    -fence_uy 0.0 \
    -target_util 0.4 \
    -keep_clustering_data
}

set out_csv ./reports/nangate45/mempool_group/base/rtlmp_cluster_groups.csv
set out_txt ./reports/nangate45/mempool_group/base/rtlmp_cluster_membership.txt
set out_flat ./reports/nangate45/mempool_group/base/rtlmp_instance_to_cluster.txt
set block [ord::get_db_block]

proc csv_escape {s} {
  set t [string map {\" \"\"} $s]
  return "\"$t\""
}

proc dump_group_recursive {group parent_name depth fcsv ftxt} {
  set name [$group getName]
  set type [$group getType]
  set insts [$group getInsts]
  set children [$group getGroups]

  puts $fcsv "[csv_escape $name],[csv_escape $parent_name],[csv_escape $type],$depth,[llength $insts],[llength $children]"

  puts $ftxt "GROUP: $name"
  puts $ftxt "PARENT: $parent_name"
  puts $ftxt "TYPE: $type"
  puts $ftxt "DEPTH: $depth"
  puts $ftxt "DIRECT_INST_COUNT: [llength $insts]"
  puts $ftxt "CHILD_GROUP_COUNT: [llength $children]"
  puts $ftxt "DIRECT_INSTS:"
  foreach inst $insts {
    puts $ftxt "  [$inst getName]"
  }
  puts $ftxt [string repeat "-" 80]

  foreach child $children {
    dump_group_recursive $child $name [expr {$depth + 1}] $fcsv $ftxt
  }
}

# Build a flat map: instance -> most specific (deepest) VISUAL_DEBUG group id.
proc collect_group_membership {group depth group_name_to_id_var inst_best_depth_name_var} {
  upvar 1 $group_name_to_id_var group_name_to_id
  upvar 1 $inst_best_depth_name_var inst_best

  set gname [$group getName]
  if {![dict exists $group_name_to_id $gname]} {
    set gid [dict size $group_name_to_id]
    dict set group_name_to_id $gname $gid
  }
  set gid [dict get $group_name_to_id $gname]

  foreach inst [$group getInsts] {
    set iname [$inst getName]
    if {![dict exists $inst_best $iname]} {
      dict set inst_best $iname [list $depth $gid $gname]
    } else {
      lassign [dict get $inst_best $iname] best_depth best_gid best_gname
      if {$depth > $best_depth} {
        dict set inst_best $iname [list $depth $gid $gname]
      }
    }
  }

  foreach child [$group getGroups] {
    collect_group_membership $child [expr {$depth + 1}] group_name_to_id inst_best
  }
}

set fcsv [open $out_csv w]
puts $fcsv "group_name,parent_group,group_type,depth,direct_inst_count,child_group_count"

set ftxt [open $out_txt w]

set groups [$block getGroups]
set top_groups {}
foreach group $groups {
  if { [$group getParentGroup] == "NULL" && [$group getType] == "VISUAL_DEBUG" } {
    lappend top_groups $group
  }
}

if { [llength $top_groups] == 0 } {
  utl::error FLW 26 \
    "rtlmp_extract: no top-level VISUAL_DEBUG groups. Use RTLMP_KEEP_CLUSTERING_DATA=1 and make do-2_2_floorplan_macro, or set RTLMP_EXTRACT_DUMP_FROM_2_2 0 to rerun from 2_1."
}

puts $ftxt "RTLMP cluster hierarchy dump"
puts $ftxt "TOP_LEVEL_GROUPS: [llength $top_groups]"
puts $ftxt [string repeat "=" 80]

foreach group $top_groups {
  dump_group_recursive $group "<block>" 0 $fcsv $ftxt
}

close $fcsv
close $ftxt

set fflat [open $out_flat w]
puts $fflat "# instance cluster_id cluster_name depth"
set group_name_to_id {}
set inst_best {}
foreach group $top_groups {
  collect_group_membership $group 0 group_name_to_id inst_best
}
set assigned 0
foreach iname [lsort -dictionary [dict keys $inst_best]] {
  lassign [dict get $inst_best $iname] d gid gname
  puts $fflat "$iname $gid $gname $d"
  incr assigned
}
close $fflat
puts "Wrote $out_flat ($assigned instances)."

if { !$RTLMP_EXTRACT_DUMP_FROM_2_2 } {
  write_db $results_dir/2_2_floorplan_macro_rtlmp_extract.odb
} else {
  puts "rtlmp_extract: dump-from-2_2 mode — reports only; left $results_dir/2_2_floorplan_macro.odb unchanged."
}
