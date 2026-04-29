# Per-instance static power (OpenSTA) and derived supply current I ~= P_total / VDD.
#
# Run from ORFS flow/:
#   make run DESIGN_CONFIG=./designs/nangate45/black_parrot/config.mk \
#       FLOW_VARIANT=base \
#       RUN_SCRIPT=./designs/nangate45/black_parrot/report_power.tcl \
#       RUN_LOG_NAME_STEM=report_power

source $::env(SCRIPTS_DIR)/load.tcl

if {![info exists ::env(REPORT_POWER_DB)]} {
  set ::env(REPORT_POWER_DB) "2_1_floorplan.odb"
}
if {![info exists ::env(REPORT_POWER_SDC)]} {
  set ::env(REPORT_POWER_SDC) "2_1_floorplan.sdc"
}
if {![info exists ::env(REPORT_POWER_VDD)]} {
  set ::env(REPORT_POWER_VDD) "1.1"
}
if {![info exists ::env(REPORT_POWER_FILTER)]} {
  set ::env(REPORT_POWER_FILTER) {is_hierarchical==0}
}
if {![info exists ::env(REPORT_POWER_CHUNK)]} {
  set ::env(REPORT_POWER_CHUNK) "8192"
}
if {![info exists ::env(REPORT_POWER_MAX_INSTANCES)]} {
  set ::env(REPORT_POWER_MAX_INSTANCES) "0"
}
if {![info exists ::env(REPORT_POWER_EST_PARASITICS)]} {
  set ::env(REPORT_POWER_EST_PARASITICS) "0"
}

load_design $::env(REPORT_POWER_DB) $::env(REPORT_POWER_SDC)

if {$::env(REPORT_POWER_EST_PARASITICS) == "1"} {
  catch { log_cmd estimate_parasitics -placement }
}

set vdd $::env(REPORT_POWER_VDD)
set cells [get_cells -hierarchical * -filter $::env(REPORT_POWER_FILTER)]
set n [llength $cells]

set maxn $::env(REPORT_POWER_MAX_INSTANCES)
if {$maxn != "" && $maxn != "0" && [string is integer -strict $maxn] && $maxn < $n} {
  set cells [lrange $cells 0 [expr {$maxn - 1}]]
  set n [llength $cells]
}

set chunk $::env(REPORT_POWER_CHUNK)
if {![string is integer -strict $chunk] || $chunk < 1} {
  set chunk 8192
}

file mkdir $::env(REPORTS_DIR)
set rpt [file join $::env(REPORTS_DIR) report_power_instances.tsv]
set tmp [file nativename [file join $::env(OBJECTS_DIR) report_power_chunk.txt]]

set out [open $rpt w]
puts $out "instance\tinternal_W\tswitching_W\tleakage_W\ttotal_W\tcurrent_A"

for {set i 0} {$i < $n} {incr i $chunk} {
  set j [expr {min($i + $chunk - 1, $n - 1)}]
  set part [lrange $cells $i $j]
  report_power -instances $part > $tmp
  set cf [open $tmp r]
  while {[gets $cf line] >= 0} {
    if {[regexp -nocase {^\s*([\d.eE+-]+|nan)\s+([\d.eE+-]+|nan)\s+([\d.eE+-]+|nan)\s+([\d.eE+-]+|nan)\s+(.+)} $line -> int sw lk tot path]} {
      foreach v {int sw lk tot} {
        if {[string match -nocase nan [set $v]]} {
          set $v 0.0
        }
      }
      set t [expr {double($tot)}]
      set cur ""
      if {[catch {expr {$t / double($vdd)}} cur]} {
        set cur ""
      }
      puts $out "$path\t$int\t$sw\t$lk\t$tot\t$cur"
    }
  }
  close $cf
}

close $out
puts "Wrote $rpt"
