if {![info exists ::env(ODB_FILE)] || $::env(ODB_FILE) eq ""} {
  error "ODB_FILE is required"
}
if {![info exists ::env(SDC_FILE)] || $::env(SDC_FILE) eq ""} {
  error "SDC_FILE is required"
}
if {![info exists ::env(PDN_NET)] || $::env(PDN_NET) eq ""} {
  set ::env(PDN_NET) "VDD"
}
if {![info exists ::env(PDN_VOLTAGE)] || $::env(PDN_VOLTAGE) eq ""} {
  set ::env(PDN_VOLTAGE) "1.1"
}
if {![info exists ::env(SOURCE_TYPE)] || $::env(SOURCE_TYPE) eq ""} {
  set ::env(SOURCE_TYPE) "FULL"
}
if {![info exists ::env(OUT_DIR)] || $::env(OUT_DIR) eq ""} {
  set ::env(OUT_DIR) "."
}

file mkdir $::env(OUT_DIR)

set net $::env(PDN_NET)
set vdd $::env(PDN_VOLTAGE)
set source_type $::env(SOURCE_TYPE)
set floorplan_conn_rpt [file join $::env(OUT_DIR) "${net}_conn_floorplan.rpt"]
set strict_conn_rpt [file join $::env(OUT_DIR) "${net}_conn_strict.rpt"]
set analyze_err_rpt [file join $::env(OUT_DIR) "${net}_analyze_err.rpt"]
set voltage_rpt [file join $::env(OUT_DIR) "${net}_instance_voltage.rpt"]

read_db $::env(ODB_FILE)
read_sdc $::env(SDC_FILE)

puts "== PDNSim floorplan-mode connectivity check =="
check_power_grid -net $net -floorplanning -dont_require_terminals -error_file $floorplan_conn_rpt

puts "== PDNSim strict connectivity check =="
check_power_grid -net $net -error_file $strict_conn_rpt

puts "== PDNSim analyze_power_grid =="
set_pdnsim_net_voltage -net $net -voltage $vdd
set analyze_ok 1
if {[info exists ::env(VSRC_FILE)] && $::env(VSRC_FILE) ne ""} {
  if {[catch {
    analyze_power_grid -net $net -source_type $source_type -vsrc $::env(VSRC_FILE) \
        -error_file $analyze_err_rpt -voltage_file $voltage_rpt
  } err]} {
    set analyze_ok 0
    puts "PDNSim analyze failed: $err"
  }
} else {
  if {[catch {
    analyze_power_grid -net $net -source_type $source_type \
        -error_file $analyze_err_rpt -voltage_file $voltage_rpt
  } err]} {
    set analyze_ok 0
    puts "PDNSim analyze failed: $err"
  }
}

puts "== Done =="
puts "floorplan check: $floorplan_conn_rpt"
puts "strict check:    $strict_conn_rpt"
puts "analyze errors:  $analyze_err_rpt"
puts "instance volts:  $voltage_rpt"
puts "analyze_ok:      $analyze_ok"
