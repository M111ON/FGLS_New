# Makefile — GPU Jet Puller Prototype
# ===========================================================================
# Targets:
#   all       — cpu-test + local + colab
#   cpu-test  — CPU-only header-validation compile check
#   local     — GTX 1050 Ti (sm_61) GPU build
#   colab     — T4 (sm_75) GPU build
#   gpu_both  — local + colab (convenience)
#   clean     — remove build artifacts
#
# Include paths (order-sensitive) are derived from the canonical source at
#   ext/dramtile_gear_build.inc  (see that file for full dependency chain
#   and include-ordering rationale).  Keep INCS in sync when the inc file
#   changes.
# ===========================================================================

NVCC := nvcc
GCC  := gcc

CFLAGS   := -O2 -std=c11
NVFLAGS  := -O2 -std=c++17

# DRamTile store needs _GNU_SOURCE on Linux for MAP_ANONYMOUS.
# The inc file documents this at ext/dramtile_gear_build.inc.
CFLAGS   += -D_GNU_SOURCE
NVFLAGS  += -D_GNU_SOURCE

# ── Include paths ─────────────────────────────────────────────────────────
# Order from ext/dramtile_gear_build.inc (critical: twin_core before
# core/core to resolve geo_fibo_clock.h from the POGLS engine directory):
#
#   -I.
#   -Icollection
#   -Icollection/src
#   -Icollection/core/pogls_engine/twin_core
#   -Icollection/core/pogls_engine
#   -Icollection/core/pogls_engine/core
#   -Icollection/core/core
#
# All paths below are relative to this Makefile's directory
# (runner/gpu_jet_puller/) and resolve to the same headers as the
# repo-root-relative paths in the inc file.
INCS := \
    -I. \
    -I../.. \
    -I../../collection \
    -I../../collection/src \
    -I../../collection/core/pogls_engine/twin_core \
    -I../../collection/core/pogls_engine \
    -I../../collection/core/pogls_engine/core \
    -I../../collection/core/core \
    -I../../runner

# ── Architecture targets ─────────────────────────────────────────────────
LOCAL_ARCH := -arch=sm_61
COLAB_ARCH := -arch=sm_75

# Host compiler for nvcc on WSL (empty = use nvcc default)
NVCC_CCBIN :=

# ── Targets ───────────────────────────────────────────────────────────────

all: cpu-test local colab

# CPU-only header validation — verifies the include chain compiles
# without CUDA.  Binary name cpu_test kept for backward compat.
cpu-test: verify_headers.c
	$(GCC) $(CFLAGS) $(INCS) -o cpu_test verify_headers.c -lm
	./cpu_test

# Local GPU — GTX 1050 Ti (sm_61)
local: gpu_jet_puller.cu
	$(NVCC) $(NVFLAGS) $(LOCAL_ARCH) $(INCS) \
		-o gpu_jet_puller_local gpu_jet_puller.cu
	@echo "=== Local build OK ==="

# Colab GPU — T4 (sm_75)
colab: gpu_jet_puller.cu
	$(NVCC) $(NVFLAGS) $(COLAB_ARCH) $(INCS) \
		-o gpu_jet_puller_colab gpu_jet_puller.cu
	@echo "=== Colab build OK ==="

# Both GPU architectures (convenience)
gpu_both: local colab

# Clean build artifacts
clean:
	rm -f cpu_test gpu_jet_puller_local gpu_jet_puller_colab *.o

.PHONY: all cpu-test local colab gpu_both clean
