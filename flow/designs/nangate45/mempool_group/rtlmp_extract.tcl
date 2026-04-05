read_liberty ./platforms/nangate45/lib/NangateOpenCellLibrary_typical.lib
read_liberty ./platforms/nangate45/lib/fakeram45_256x32.lib
read_liberty ./platforms/nangate45/lib/fakeram45_64x64.lib

read_db ./results/nangate45/mempool_group/base/2_1_floorplan.odb
read_sdc ./results/nangate45/mempool_group/base/2_1_floorplan.sdc
source ./platforms/nangate45/setRC.tcl

file mkdir ./objects/nangate45/mempool_group/base/rtlmp_extract
file mkdir ./reports/nangate45/mempool_group/base

# Enable RTLMP debug floorplan dumps (.fp.txt / .cost.txt / .net.txt).
set_debug_level MPL hierarchical_macro_placement 1

rtl_macro_placer \
  -max_num_level 2 \
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
  -target_util 0.3 \
  -keep_clustering_data

set out_csv ./reports/nangate45/mempool_group/base/rtlmp_cluster_groups.csv
set out_txt ./reports/nangate45/mempool_group/base/rtlmp_cluster_membership.txt
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

puts $ftxt "RTLMP cluster hierarchy dump"
puts $ftxt "TOP_LEVEL_GROUPS: [llength $top_groups]"
puts $ftxt [string repeat "=" 80]

foreach group $top_groups {
  dump_group_recursive $group "<block>" 0 $fcsv $ftxt
}

close $fcsv
close $ftxt

write_db ./results/nangate45/mempool_group/base/2_2_floorplan_macro_rtlmp_extract.odb
