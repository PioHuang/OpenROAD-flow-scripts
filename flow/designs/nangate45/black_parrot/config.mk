export DESIGN_NICKNAME = bp
export DESIGN_NAME = black_parrot
export PLATFORM    = nangate45

export SYNTH_HIERARCHICAL = 1
#

export VERILOG_FILES = $(DESIGN_HOME)/src/$(DESIGN_NAME)/pickled.v \
                       $(DESIGN_HOME)/$(PLATFORM)/$(DESIGN_NAME)/macros.v

export ABC_AREA = 1

export SDC_FILE      = $(DESIGN_HOME)/$(PLATFORM)/$(DESIGN_NAME)/constraint.sdc

export ADDITIONAL_LEFS = $(PLATFORM_DIR)/lef/fakeram45_512x64.lef \
                         $(PLATFORM_DIR)/lef/fakeram45_256x95.lef \
                         $(PLATFORM_DIR)/lef/fakeram45_64x7.lef \
                         $(PLATFORM_DIR)/lef/fakeram45_64x15.lef \
                         $(PLATFORM_DIR)/lef/fakeram45_64x96.lef
export ADDITIONAL_LIBS = $(PLATFORM_DIR)/lib/fakeram45_512x64.lib \
                         $(PLATFORM_DIR)/lib/fakeram45_256x95.lib \
                         $(PLATFORM_DIR)/lib/fakeram45_64x7.lib \
                         $(PLATFORM_DIR)/lib/fakeram45_64x15.lib \
                         $(PLATFORM_DIR)/lib/fakeram45_64x96.lib


export DIE_AREA    = 0 0 1350 1300 
export CORE_AREA   = 10.07 11.2 1340 1290 

export IO_CONSTRAINTS = $(DESIGN_HOME)/$(PLATFORM)/$(DESIGN_NAME)/io.tcl

export PLACE_DENSITY_LB_ADDON = 0.05

export MACRO_PLACEMENT_TCL = $(DESIGN_HOME)/$(PLATFORM)/$(DESIGN_NAME)/macro_placement.tcl

export MACRO_PLACE_HALO    = 10 10

export TNS_END_PERCENT     = 100

export HOLD_SLACK_MARGIN = 0.03

export SYNTH_KEEP_MODULES = \
  bp_be_calculator_top_vaddr_width_p56_paddr_width_p22_asid_width_p10_branch_metadata_fwd_width_p36_core_els_p1_num_lce_p2_lce_sets_p64_cce_block_size_in_bytes_p64 \
  bp_be_dcache_data_width_p64_paddr_width_p22_sets_p64_ways_p8_num_cce_p1_num_lce_p2 \
  bp_be_dcache_lce_data_width_p64_paddr_width_p22_lce_data_width_p512_sets_p64_ways_p8_num_cce_p1_num_lce_p2 \
  bp_cce_num_lce_p2_num_cce_p1_paddr_width_p22_lce_assoc_p8_lce_sets_p64_block_size_in_bytes_p64_num_cce_inst_ram_els_p256 \
  bp_coherence_network_num_lce_p2_num_cce_p1_paddr_width_p22_lce_assoc_p8_block_size_in_bytes_p64 \
  bp_core_core_els_p1_num_lce_p2_num_cce_p1_lce_assoc_p8_lce_sets_p64_cce_block_size_in_bytes_p64_vaddr_width_p56_paddr_width_p22_branch_metadata_fwd_width_p36_asid_width_p10_btb_indx_width_p9_bht_indx_width_p5_ras_addr_width_p22 \
  bsg_mem_1rw_sync_mask_write_bit_width_p192_els_p64 \
  bsg_mesh_router_539_1_1_0_0a_0 \
  bsg_mesh_router_539_1_1_0_0d_0 \
  bsg_mesh_router_541_1_1_0_0a_0 \
  bsg_mesh_router_541_1_1_0_0c_0 \
  bsg_mesh_router_542_1_1_0_0a_0 \
  bsg_mesh_router_542_1_1_0_1c_0 \
  hard_mem_1rw_byte_mask_d512_w64_wrapper \
  hard_mem_1rw_d256_w95_wrapper \
  hard_mem_1rw_d512_w64_wrapper \
  icache_eaddr_width_p64_data_width_p64_inst_width_p32_tag_width_p10_num_cce_p1_num_lce_p2_ways_p8_lce_sets_p64_block_size_in_bytes_p8

# Keep RTLMP artifacts available when macro placement is used.
export RTLMP_KEEP_CLUSTERING_DATA = 1
export RTLMP_DEBUG_FLOORPLAN = 1
export RTLMP_RPT_DIR = $(OBJECTS_DIR)/rtlmp_extract

# Research flow support: export PDNSim-style source geometry and real PDN via sites
# at floorplan/PDN stage for research/phys_load.
export EXPORT_PSM_VSRC = 1
export EXPORT_PDN_VIAS = 1
