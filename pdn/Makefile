SHELL := /usr/bin/env bash

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -Iinclude
BUILD_DIR ?= build
TARGET ?= $(BUILD_DIR)/pdn_demo

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all run clean

all: $(TARGET)

run: $(TARGET)
	@$(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CXXFLAGS) $(OBJS) -o "$@"

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"

clean:
	rm -rf "$(BUILD_DIR)"

-include $(DEPS)
