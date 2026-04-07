export DESIGN_NAME = mempool_group
export DESIGN_NICKNAME = mempool_group
export PLATFORM    = nangate45

export SYNTH_HIERARCHICAL = 1
# Keep flattened netlist for stable OpenROAD placement, but preserve hierarchy
# information in flattened instance names using SYNTH_HIER_SEPARATOR.
export SYNTH_PRESERVE_RTL_HIERARCHY = 1
export SYNTH_HIER_SEPARATOR = /
export OPENROAD_HIERARCHICAL = 0

export TEMP_DESIGN_DIR = $(DESIGN_HOME)/$(PLATFORM)/$(DESIGN_NICKNAME)

export VERILOG_FILES = $(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/axi/src/axi_pkg.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/cf_math_pkg.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/riscv_instr.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_pkg.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/mempool_pkg.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/cluster_interconnect/rtl/tcdm_interconnect/tcdm_interconnect_pkg.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/cluster_interconnect/rtl/variable_latency_interconnect/variable_latency_interconnect.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/axi_hier_interco.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/mempool_tile.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/axi/src/axi_mux.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/axi/src/axi_id_remap.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/mempool_cc.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_icache/snitch_icache_pkg.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_icache/snitch_icache.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/tcdm_adapter.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/fakeram45_256x32.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/fakeram45_64x64.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/tech_cells_generic/src/rtl/tc_sram.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/spill_register.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/fall_through_register.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/stream_xbar.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/address_scrambler.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/tcdm_shim.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_demux.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_axi_adapter.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/axi/src/axi_cut.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/axi/src/axi_id_prepend.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/rr_arb_tree.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/fifo_v3.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/lzc.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_ipu.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/isochronous_spill_register.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_icache/snitch_icache_lookup.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_icache/snitch_icache_l0.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/stream_arbiter.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_icache/snitch_icache_refill.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/onehot_to_bin.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/stream_demux.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/deprecated/fifo_v2.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/spill_register_flushable.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/stream_arbiter_flushable.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/latch_scm.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_lsu.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/tech_cells_generic/src/rtl/tc_clk.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_icache/snitch_icache_handler.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch_addr_demux.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_regfile_ff.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_shared_muldiv.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/common_cells/src/deprecated/find_first_one.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_onehot.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/snitch/src/snitch_icache/snitch_icache_lfsr.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/axi/src/axi_intf.sv \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/mempool_group.sv

export VERILOG_INCLUDE_DIRS = $(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl \
	$(DESIGN_HOME)/src/$(DESIGN_NICKNAME)/rtl/register_interface/include

export SDC_FILE      = $(DESIGN_HOME)/$(PLATFORM)/$(DESIGN_NICKNAME)/$(DESIGN_NAME).sdc

export ADDITIONAL_LEFS = $(PLATFORM_DIR)/lef/fakeram45_256x32.lef \
                         $(PLATFORM_DIR)/lef/fakeram45_64x64.lef

export ADDITIONAL_LIBS = $(PLATFORM_DIR)/lib/fakeram45_256x32.lib \
                         $(PLATFORM_DIR)/lib/fakeram45_64x64.lib

export DIE_AREA    = 0 0 1100 1100
export CORE_AREA   = 10 12 1090 1090

export MACRO_PLACE_HALO = 10 10

export SYNTH_HDL_FRONTEND = slang

export LEC_CHECK = 0

# Routability / timing (global_place.tcl lines 27–35)
export GPL_ROUTABILITY_DRIVEN = 0
export GPL_TIMING_DRIVEN = 0

# --- Optional placement tuning (see flow/scripts/global_place.tcl, detail_place.tcl) ---
# Extra flags appended to global_placement (OpenROAD replace.tcl for full list).
export GLOBAL_PLACEMENT_ARGS = -disable_pin_density_adjust

# Skip improve_placement after detailed_placement (reduces post-GPL drift, esp. small islands).
# export ENABLE_DPO = 0
# If ENABLE_DPO = 1, cap motion in microns:
# export DPO_MAX_DISPLACEMENT = 5

# Nudge density / padding (defaults come from variables.mk; uncomment to override).
export PLACE_DENSITY = 0.3
# export CELL_PAD_IN_SITES_GLOBAL_PLACEMENT = 0

# --- RTLMP cluster sizing (macro_place_util.tcl → rtl_macro_placer); rerun floorplan after change ---
# Fewer tiny soft clusters: raise min std cells per cluster (tune to your design).
# export RTLMP_MIN_INST = 500
# Optional: stronger parent-outline pressure (default 100 in variables.yaml).
# export RTLMP_OUTLINE_WT = 150
# Override all RTLMP args in one string (replaces individual RTLMP_* passthrough).
# export RTLMP_ARGS = {-max_num_level 2 ... }
