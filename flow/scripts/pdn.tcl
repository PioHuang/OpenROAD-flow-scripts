source $::env(SCRIPTS_DIR)/load.tcl
erase_non_stage_variables floorplan
load_design 2_3_floorplan_tapcell.odb 2_1_floorplan.sdc
source_step_tcl PRE PDN

source $::env(PDN_TCL)
pdngen

source_step_tcl POST PDN

# Check all supply nets
set block [ord::get_db_block]
foreach net [$block getNets] {
  set type [$net getSigType]
  if { $type == "POWER" || $type == "GROUND" } {
    # Temporarily disable due to CI issues
    #        puts "Check supply: [$net getName]"
    #        check_power_grid -net [$net getName]
  }
}

report_design_area

# Optional: dump PSM `-vsrc` geometry from IO/package terminals (floorplan / PDN stage).
# Research flows that stop before route should use this, not finish `final_report.tcl`.
if { [env_var_equals EXPORT_PSM_VSRC 1] } {
  source $::env(SCRIPTS_DIR)/export_psm_vsrc.tcl
  export_psm_vsrc_from_bterms $::env(REPORTS_DIR)
}

# Optional: dump real PDN via locations (from special-net dbSBox vias) at floorplan/PDN stage.
if { [env_var_equals EXPORT_PDN_VIAS 1] } {
  source $::env(SCRIPTS_DIR)/export_pdn_vias.tcl
  export_pdn_vias_from_special_nets $::env(REPORTS_DIR)
}

# Optional: dump strap / non-via special wire boxes (same nets as EXPORT_PDN_VIAS).
if { [env_var_equals EXPORT_PDN_WIRES 1] } {
  source $::env(SCRIPTS_DIR)/export_pdn_wires.tcl
  export_pdn_wires_from_special_nets $::env(REPORTS_DIR)
}

orfs_write_db $::env(RESULTS_DIR)/2_4_floorplan_pdn.odb

# Optional second ODB with soft module dbRegion/dbGroup for floorplan GUI review only.
# Placement must load the clean 2_4_floorplan_pdn.odb (no regions) to avoid GPL-0301.
if { ![env_var_equals WRITE_SOFT_REGION_FLOORPLAN_PREVIEW 0] } {
  if { [info exists ::env(SOFT_MODULE_REGIONS_TCL)] && $::env(SOFT_MODULE_REGIONS_TCL) != "" } {
    set sr $::env(SOFT_MODULE_REGIONS_TCL)
    if { [file exists $sr] && [file size $sr] > 0 } {
      source $::env(SCRIPTS_DIR)/soft_module_regions.tcl
      source $sr
      orfs_write_db $::env(RESULTS_DIR)/2_4_floorplan_pdn_soft_preview.odb
      puts "Wrote soft-region floorplan preview: $::env(RESULTS_DIR)/2_4_floorplan_pdn_soft_preview.odb"
    }
  }
}
