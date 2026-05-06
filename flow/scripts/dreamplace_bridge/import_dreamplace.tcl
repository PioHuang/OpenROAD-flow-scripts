#!/usr/bin/env openroad
#
# import_dreamplace.tcl
# Import DREAMPlace placement results back into the OpenROAD floorplan ODB.
#
# Loads the original floorplan ODB (with PDN, tapcells, etc.), then reads
# the DREAMPlace output DEF to update component placement coordinates.
#
# Usage:
#   RESULTS_DIR=<path> DREAMPLACE_DEF=<dp_output.def> \
#       openroad -no_splash import_dreamplace.tcl
#
# Inputs:
#   $RESULTS_DIR/2_floorplan.odb  -- original floorplan database
#   $RESULTS_DIR/2_floorplan.sdc  -- timing constraints
#   $DREAMPLACE_DEF               -- DREAMPlace placed DEF output
#
# Outputs:
#   $RESULTS_DIR/3_1_place_gp_skip_io.odb  -- placement result for CTS stage
#   $RESULTS_DIR/3_1_place_gp_skip_io.sdc  -- copied SDC

set results_dir $::env(RESULTS_DIR)
set dp_def      $::env(DREAMPLACE_DEF)

puts "Loading original floorplan ODB..."
read_db ${results_dir}/2_floorplan.odb

puts "Parsing DREAMPlace placement from: ${dp_def}"

# Parse the DREAMPlace output DEF for component placements.
# Build a dict: instance_name -> {x y orient status}
set placements [dict create]
set in_components 0
set fp [open $dp_def r]
while {[gets $fp line] >= 0} {
    set line [string trim $line]

    if {[string match "COMPONENTS *" $line]} {
        set in_components 1
        continue
    }
    if {$line eq "END COMPONENTS"} {
        set in_components 0
        continue
    }

    if {!$in_components} {
        continue
    }

    # DEF COMPONENTS entries look like:
    #   - inst_name cell_type + PLACED ( x y ) orient ;
    #   - inst_name cell_type + FIXED ( x y ) orient ;
    # May span multiple lines; accumulate until we see the semicolon.
    if {[string index $line 0] eq "-"} {
        set comp_line $line
    } else {
        append comp_line " " $line
    }

    if {![string match "*;" $comp_line]} {
        continue
    }

    # Parse the accumulated component line
    if {[regexp -- {- (\S+) \S+ \+ (PLACED|FIXED|COVER|UNPLACED) \( (-?\d+) (-?\d+) \) (\S+)} $comp_line -> inst_name status x y orient]} {
        if {$status ne "FIXED" && $status ne "COVER"} {
            dict set placements $inst_name [list $x $y $orient]
        }
    }
}
close $fp

set num_updates [dict size $placements]
puts "Found $num_updates movable component placements to import"

# Apply placements to the ODB
set block [ord::get_db_block]
set update_count 0
set missing_count 0

dict for {inst_name coords} $placements {
    set inst [$block findInst $inst_name]
    if {$inst eq "NULL"} {
        incr missing_count
        continue
    }
    lassign $coords x y orient
    $inst setLocation $x $y
    $inst setPlacementStatus "PLACED"
    incr update_count
}

puts "Updated $update_count instances"
if {$missing_count > 0} {
    puts "WARNING: $missing_count instances from DREAMPlace DEF not found in ODB"
}

# Write results
puts "Writing placement ODB..."
write_db ${results_dir}/3_1_place_gp_skip_io.odb

puts "Copying SDC..."
file copy -force ${results_dir}/2_floorplan.sdc ${results_dir}/3_1_place_gp_skip_io.sdc

puts "Import complete."
puts "  ODB: ${results_dir}/3_1_place_gp_skip_io.odb"
puts "  SDC: ${results_dir}/3_1_place_gp_skip_io.sdc"
exit
