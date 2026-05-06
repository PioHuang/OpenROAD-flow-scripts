#!/usr/bin/env openroad
#
# export_for_dreamplace.tcl
# Export DEF + Verilog from OpenROAD floorplan ODB for DREAMPlace consumption.
#
# Usage:
#   RESULTS_DIR=<path> openroad -no_splash export_for_dreamplace.tcl
#
# Inputs:  $RESULTS_DIR/2_floorplan.odb, $RESULTS_DIR/2_floorplan.sdc
# Outputs: $RESULTS_DIR/2_floorplan_for_dp.def, $RESULTS_DIR/2_floorplan_for_dp.v

set results_dir $::env(RESULTS_DIR)

puts "Reading floorplan ODB..."
read_db ${results_dir}/2_floorplan.odb

puts "Writing DEF for DREAMPlace..."
write_def ${results_dir}/2_floorplan_for_dp.def

puts "Writing Verilog for DREAMPlace..."
write_verilog ${results_dir}/2_floorplan_for_dp.v

puts "Export complete."
puts "  DEF:     ${results_dir}/2_floorplan_for_dp.def"
puts "  Verilog: ${results_dir}/2_floorplan_for_dp.v"
exit
