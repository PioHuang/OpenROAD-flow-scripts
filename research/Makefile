# Wrapper around CMake: from this directory, `make` configures (if needed) and builds.
# Override defaults: make BUILD=debug-build CMAKE=cmake

CMAKE ?= cmake
BUILD ?= build
JOBS  ?=

.PHONY: all configure clean distclean

all: configure
	$(CMAKE) --build $(BUILD) $(if $(JOBS),-j $(JOBS),)

# Re-run CMake when the project file changes.
configure: $(BUILD)/CMakeCache.txt

$(BUILD)/CMakeCache.txt: CMakeLists.txt
	$(CMAKE) -S . -B $(BUILD)

clean:
	@if [ -f $(BUILD)/CMakeCache.txt ]; then \
		$(CMAKE) --build $(BUILD) --target clean; \
	fi

distclean:
	rm -rf $(BUILD)
