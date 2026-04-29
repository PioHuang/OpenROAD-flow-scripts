SHELL := /usr/bin/env bash

OPENROAD ?= ../tools/install/OpenROAD/bin/openroad
ODB ?= ../flow/results/nangate45/mempool_group/rtlmp/2_floorplan.odb
SDC ?= ../flow/results/nangate45/mempool_group/rtlmp/2_floorplan.sdc
OUT ?= ./out
NET ?= VDD
VOLT ?= 1.1
SOURCE ?= FULL
VSRC ?=
SHAPES ?= ./out/db_pdn_shapes.tsv
VIAS ?= ./out/pdn_vias_$(NET).tsv
EXTRACT_BIN ?= ./build/pdn_extract
EXTRACT_OUT ?= ./out/pdn_extract_report.tsv

.PHONY: all build run prep extract_build extract clean

all: build

build:
	@test -x "$(OPENROAD)" || (echo "openroad not executable at $(OPENROAD)"; exit 1)
	@chmod +x "./run_pdnsim_floorplan.sh"
	@chmod +x "./extract_floorplan_db.sh"
	@mkdir -p "$(OUT)"
	@echo "PDNSim runner ready."

run: build
	@OPENROAD_BIN="$(OPENROAD)" ./run_pdnsim_floorplan.sh "$(ODB)" "$(SDC)" "$(OUT)" "$(NET)" "$(VOLT)" "$(SOURCE)" "$(VSRC)"

clean:
	@rm -rf "$(OUT)"/*
	@echo "Cleaned $(OUT)"

prep: build
	@OPENROAD_BIN="$(OPENROAD)" ./extract_floorplan_db.sh "$(ODB)" "$(SDC)" "$(OUT)"

extract_build:
	@mkdir -p ./build
	@g++ -std=c++17 -O2 -o "$(EXTRACT_BIN)" ./src/pdn_extract_main.cpp
	@echo "Built $(EXTRACT_BIN)"

extract: prep extract_build
	@"$(EXTRACT_BIN)" "$(SHAPES)" "$(VIAS)" "$(NET)" "$(EXTRACT_OUT)"
