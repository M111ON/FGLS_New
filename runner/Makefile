# ═══════════════════════════════════════════════════════════════════════
# POGLS Toolchain — Cross-Platform Makefile (Windows/MinGW + Linux/WSL)
# ═══════════════════════════════════════════════════════════════════════

# ── OS detection ──────────────────────────────────────────────────────
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifneq (,$(findstring Linux,$(UNAME_S)))
  IS_LINUX := 1
  EXE      :=
  DLLEXT   := .so
  LIBPFX   := lib
  STACKFLAG :=
  RM       := rm -f
  NULLDEV  := /dev/null
  ZSTDLIB  := -lzstd
  LDLIBS_EXTRA := -lpthread -ldl
  LLAMA_DIR ?= /usr/local/llama.cpp
else
  IS_LINUX := 0
  EXE      := .exe
  DLLEXT   := .dll
  LIBPFX   := lib
  STACKFLAG := -Wl,--stack,16777216
  RM       := rm -f
  NULLDEV  := nul
  ZSTDLIB  := ./zstd.dll
  LDLIBS_EXTRA :=
  LLAMA_DIR ?= I:/llama.cpp
endif

# ── Tools & Flags ─────────────────────────────────────────────────────
CC       = gcc
CXX      = g++
INCS     = -I. -I../collection -I../collection/src -I../collection/core
INCS    += -I../collection/core/core -I../collection/geopixel
INCS    += -I../collection/geopixel/Metatron/core -I../collection/pogls_engine
INCS    += -I../collection/geo_jump_module/include
INCS    += -I../collection/geopixel/hbv_bundle/Diamond_shell_encoder
INCS    += -I../collection/geopixel/hbv_bundle/Diamond_decode_hamburger
INCS    += -I../collection/geopixel/hbv_bundle/core
INCS    += -I../collection/Hfolder
INCS    += -I../collection/rdh
INCS    += -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
INCS    += -I$(LLAMA_DIR)/src
INCS    += -Ipogls_core -Ipogls_gguf -Ipogls_geo -Ipogls_dram -Ipogls_bermuda -Ipogls_kv -Ipogls_geopixel
CFLAGS   = -O2 -std=c11 -fno-strict-aliasing $(INCS)
CXXFLAGS = -O2 -std=c++17 $(INCS)
LDLIBS   = -lm $(LDLIBS_EXTRA)

INSTALL_DIR ?= $(PWD)/bin
TEST_VERIFY ?= ../test_verify.pogls

# ── DLL / .so paths (runner only) ─────────────────────────────────────
DLLDIR    = $(LLAMA_DIR)/llama-b9733-bin-win-vulkan-x64
DLLS      = $(DLLDIR)/llama$(DLLEXT) $(DLLDIR)/ggml$(DLLEXT) \
            $(DLLDIR)/ggml-base$(DLLEXT) $(DLLDIR)/ggml-cpu-x64$(DLLEXT) \
            $(DLLDIR)/ggml-vulkan$(DLLEXT)
RUNNER_LIBS = -L. -llibllama -lggml -lggml-base -lggml-cpu-x64 -lggml-vulkan -lstdc++

.PHONY: all core modules tools test test_modules install clean help

# ═══════════════════════════════════════════════════════════════════════
# Default
# ═══════════════════════════════════════════════════════════════════════
all: core modules tools

# ═══════════════════════════════════════════════════════════════════════
# POGLS Core Library
# ═══════════════════════════════════════════════════════════════════════
POGLS_CORE_OBJS = pogls_core/pogls_platform.o pogls_core/pogls_compress.o \
                  pogls_core/pogls_addr.o pogls_core/pogls_meta.o

core: pogls_core.lib

$(POGLS_CORE_OBJS): pogls_core/pogls_platform.h pogls_core/pogls_core.h

pogls_core/pogls_platform.o: pogls_core/pogls_platform.c
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_core/pogls_compress.o: pogls_core/pogls_compress.c pogls_core/pogls_compress.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -c -o $@ $<

pogls_core/pogls_addr.o: pogls_core/pogls_addr.c pogls_core/pogls_addr.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_core/pogls_meta.o: pogls_core/pogls_meta.c pogls_core/pogls_meta.h pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_core.lib: $(POGLS_CORE_OBJS)
	ar rcs $@ $^

# ═══════════════════════════════════════════════════════════════════════
# POGLS Standalone Modules
# ═══════════════════════════════════════════════════════════════════════
POGLS_MODULE_OBJS = pogls_gguf/pogls_gguf.o pogls_geo/pogls_geo.o \
                    pogls_dram/pogls_dram.o pogls_bermuda/pogls_bermuda.o \
                    pogls_kv/pogls_kv.o pogls_geopixel/pogls_geopixel.o

modules: $(POGLS_MODULE_OBJS)

pogls_gguf/pogls_gguf.o: pogls_gguf/pogls_gguf.c pogls_gguf/pogls_gguf.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_geo/pogls_geo.o: pogls_geo/pogls_geo.c pogls_geo/pogls_geo.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_dram/pogls_dram.o: pogls_dram/pogls_dram.c pogls_dram/pogls_dram.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_bermuda/pogls_bermuda.o: pogls_bermuda/pogls_bermuda.c pogls_bermuda/pogls_bermuda.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_kv/pogls_kv.o: pogls_kv/pogls_kv.c pogls_kv/pogls_kv.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_geopixel/pogls_geopixel.o: pogls_geopixel/pogls_geopixel.c pogls_geopixel/pogls_geopixel.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ═══════════════════════════════════════════════════════════════════════
# Module Tests
# ═══════════════════════════════════════════════════════════════════════
test_modules: core modules
	@echo "=== POGLS Module Tests ==="
	@$(CC) $(CFLAGS) -o _test_core$(EXE) pogls_core/test_pogls_core.c $(POGLS_CORE_OBJS) $(ZSTDLIB) -lm && ./_test_core$(EXE) && $(RM) _test_core$(EXE)
	@$(CC) $(CFLAGS) -o _test_gguf$(EXE) pogls_gguf/test_pogls_gguf.c pogls_gguf/pogls_gguf.o && ./_test_gguf$(EXE) && $(RM) _test_gguf$(EXE)
	@$(CC) $(CFLAGS) -o _test_geo$(EXE) pogls_geo/test_pogls_geo.c pogls_geo/pogls_geo.o -lm && ./_test_geo$(EXE) && $(RM) _test_geo$(EXE)
	@$(CC) $(CFLAGS) -o _test_dram$(EXE) pogls_dram/test_pogls_dram.c pogls_dram/pogls_dram.o pogls_core/pogls_platform.o && ./_test_dram$(EXE) && $(RM) _test_dram$(EXE)
	@$(CC) $(CFLAGS) -o _test_bermuda$(EXE) pogls_bermuda/test_pogls_bermuda.c pogls_bermuda/pogls_bermuda.o pogls_core/pogls_platform.o && ./_test_bermuda$(EXE) && $(RM) _test_bermuda$(EXE)
	@$(CC) $(CFLAGS) -o _test_kv$(EXE) pogls_kv/test_pogls_kv.c pogls_kv/pogls_kv.o && ./_test_kv$(EXE) && $(RM) _test_kv$(EXE)
	@$(CC) $(CFLAGS) -o _test_geopixel$(EXE) pogls_geopixel/test_pogls_geopixel.c pogls_geopixel/pogls_geopixel.o -lm && ./_test_geopixel$(EXE) && $(RM) _test_geopixel$(EXE)
	@echo "=== All module tests done ==="

# ═══════════════════════════════════════════════════════════════════════
# POGLS CLI Tools
# ═══════════════════════════════════════════════════════════════════════
TOOLS = pogls_compress$(EXE) pogls_decompress$(EXE) pogls_inspect$(EXE) \
        pogls_roundtrip$(EXE) addr_resolve$(EXE) gguf_dump$(EXE) \
        dramtile_dump$(EXE) dramtile_bench$(EXE) dramtile_cli$(EXE) \
        kv_delta_bench$(EXE) kv_delta_test$(EXE) \
        pogls_verify$(EXE) pogls_build$(EXE) pogls_cat$(EXE) \
        pogls_diff$(EXE) pogls_test$(EXE) \
        test_pogls_loader$(EXE) pogls_loader_demo$(EXE)

tools: core $(TOOLS)

# Phase 1: Core CLI
pogls_compress$(EXE): pogls_tools/pogls_compress.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) $(LDLIBS)

pogls_decompress$(EXE): pogls_tools/pogls_decompress.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) $(LDLIBS)

pogls_inspect$(EXE): pogls_tools/pogls_inspect.c pogls_core/pogls_core.h pogls_meta.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core $(LDLIBS)

pogls_roundtrip$(EXE): pogls_tools/pogls_roundtrip.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) $(LDLIBS)

# Phase 2: Address + GGUF
addr_resolve$(EXE): pogls_tools/addr_resolve.c pogls_core/pogls_addr.h gguf_reader.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core $(LDLIBS)

gguf_dump$(EXE): pogls_tools/gguf_dump.c gguf_reader.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Phase 3: Store + Delta
dramtile_dump$(EXE): pogls_tools/dramtile_dump.c dramtile_store.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

dramtile_bench$(EXE): pogls_tools/dramtile_bench.c dramtile_store.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

dramtile_cli$(EXE): pogls_tools/dramtile_cli.c dramtile_store.h pogls_core/pogls_platform.h ../collection/geo_dram_tile.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core $(LDLIBS)

kv_delta_bench$(EXE): pogls_tools/kv_delta_bench.c kv_remap.h kv_remap_diamond.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

kv_delta_test$(EXE): pogls_tools/kv_delta_test.c kv_remap.h kv_remap_diamond.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Phase 4: Pipeline
pogls_verify$(EXE): pogls_tools/pogls_verify.c pogls_core/pogls_core.h pogls_meta.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core $(LDLIBS)

pogls_build$(EXE): pogls_tools/pogls_build.c pogls_core/pogls_core.h pogls_meta.h gguf_reader.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core $(LDLIBS)

pogls_cat$(EXE): pogls_tools/pogls_cat.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core $(LDLIBS)

pogls_diff$(EXE): pogls_tools/pogls_diff.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core $(LDLIBS)

pogls_test$(EXE): pogls_tools/pogls_test.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) $(LDLIBS)

# ── POGLS Loader (standalone) ──
test_pogls_loader$(EXE): pogls_tools/test_pogls_loader.c pogls_core/pogls_loader.h
	$(CC) $(CFLAGS) -DPOGLS_LOADER_IMPLEMENTATION -o $@ $< $(LDLIBS)

pogls_loader_demo$(EXE): pogls_tools/pogls_loader_demo.c pogls_core/pogls_loader.h
	$(CC) $(CFLAGS) -DPOGLS_LOADER_IMPLEMENTATION -o $@ $< $(LDLIBS)

libpogls_loader.a: pogls_core/pogls_loader.o
	ar rcs $@ $^

pogls_core/pogls_loader.o: pogls_core/pogls_loader.c pogls_core/pogls_loader.h
	$(CC) $(CFLAGS) -DPOGLS_LOADER_IMPLEMENTATION -c -o $@ $<

# ═══════════════════════════════════════════════════════════════════════
# Runner (requires llama.cpp DLLs/SOs)
# ═══════════════════════════════════════════════════════════════════════
runner: llama_pogls_runner_sid_v2$(EXE) gguf_to_pogls$(EXE)

llama_pogls_runner_sid_v2$(EXE): llama_pogls_runner_sid_v2.o geo_jump.o kv_tensor_access.o
	$(CC) -m64 -O2 $(STACKFLAG) -o $@ $^ $(RUNNER_LIBS) -lstdc++ $(LDLIBS) $(ZSTDLIB)

llama_pogls_runner_sid_v2.o: llama_pogls_runner_sid_v2.c
	$(CC) $(CFLAGS) -c -o $@ $<

gguf_to_pogls$(EXE): gguf_to_pogls.c pogls_meta.h gguf_index.h addr_space.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< $(ZSTDLIB) $(LDLIBS)

geo_jump.o: ../collection/geo_jump_module/src/geo_jump.c
	$(CC) $(CFLAGS) -c -o $@ $<

kv_tensor_access.o: kv_tensor_access.cpp kv_tensor_access.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ═══════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════
test_core: core
	$(CC) $(CFLAGS) -o test_pogls_core$(EXE) pogls_core/test_pogls_core.c $(POGLS_CORE_OBJS) $(ZSTDLIB) $(LDLIBS)
	./test_pogls_core$(EXE)

test: tools test_modules
	@echo "=== Running toolchain tests ==="
	@./pogls_test$(EXE) && echo "[1/7] core tests: PASS" || echo "[1/7] core tests: FAIL"
	@./pogls_roundtrip$(EXE) pogls_core/pogls_platform.h && echo "[2/7] roundtrip: PASS" || echo "[2/7] roundtrip: FAIL"
	@./pogls_verify$(EXE) $(TEST_VERIFY) && echo "[3/7] verify v1: PASS" || echo "[3/7] verify v1: FAIL (expected for v1)"
	@./pogls_cat$(EXE) _test_cat.bin pogls_core/pogls_platform.h pogls_core/pogls_compress.h && echo "[4/7] cat: PASS" || echo "[4/7] cat: FAIL"
	@./pogls_diff$(EXE) pogls_core/pogls_platform.h pogls_core/pogls_platform.h && echo "[5/7] diff identical: PASS" || echo "[5/7] diff identical: FAIL"
	@./addr_resolve$(EXE) --name "blk.0.attn_q.weight" >$(NULLDEV) 2>&1 && echo "[6/7] addr_resolve: PASS" || echo "[6/7] addr_resolve: FAIL"
	@./kv_delta_test$(EXE) 2>&1 | grep "ALL PASS" >$(NULLDEV) && echo "[7/7] kv_delta_test: PASS" || echo "[7/7] kv_delta_test: FAIL"
	@echo "=== Done ==="
	@$(RM) _test_cat.bin 2>$(NULLDEV)

# ═══════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════
install: tools
	@echo "Installing tools to $(INSTALL_DIR)..."
	@mkdir -p "$(INSTALL_DIR)"
	@cp pogls_compress$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_decompress$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_inspect$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_roundtrip$(EXE) "$(INSTALL_DIR)/"
	@cp addr_resolve$(EXE) "$(INSTALL_DIR)/"
	@cp gguf_dump$(EXE) "$(INSTALL_DIR)/"
	@cp dramtile_dump$(EXE) "$(INSTALL_DIR)/"
	@cp dramtile_bench$(EXE) "$(INSTALL_DIR)/"
	@cp kv_delta_bench$(EXE) "$(INSTALL_DIR)/"
	@cp kv_delta_test$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_verify$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_build$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_cat$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_diff$(EXE) "$(INSTALL_DIR)/"
	@cp pogls_core.lib "$(INSTALL_DIR)/"
	@echo "Installed 15 tools + pogls_core.lib to $(INSTALL_DIR)"

# ═══════════════════════════════════════════════════════════════════════
# Clean
# ═══════════════════════════════════════════════════════════════════════
clean:
	$(RM) *.o *.$(LIBPFX) *.a *.$(if $(findstring .exe,$(EXE)),exe,) \
	      pogls_core/*.o pogls_tools/*.o \
	      pogls_gguf/*.o pogls_geo/*.o pogls_dram/*.o \
	      pogls_bermuda/*.o pogls_kv/*.o pogls_geopixel/*.o
	$(RM) _test_core$(EXE) _test_gguf$(EXE) _test_geo$(EXE) _test_dram$(EXE) \
	      _test_bermuda$(EXE) _test_kv$(EXE) _test_geopixel$(EXE) 2>$(NULLDEV)
	$(RM) $(TOOLS) 2>$(NULLDEV)
	$(RM) test_pogls_core$(EXE) 2>$(NULLDEV)

# ═══════════════════════════════════════════════════════════════════════
# Help
# ═══════════════════════════════════════════════════════════════════════
help:
	@echo POGLS Toolchain -- Build Targets
	@echo ---------------------------------
	@echo   all        Build core + tools (cross-platform)
	@echo   core       Build pogls_core.lib (static)
	@echo   tools      Build all 15 CLI tools
	@echo   runner     Build llama_pogls_runner + gguf_to_pogls
	@echo   test       Run toolchain test suite
	@echo   install    Install tools to $(INSTALL_DIR)
	@echo   clean      Remove all build artifacts
	@echo
	@echo OS: $(UNAME_S)  EXE: '$(EXE)'  DLL: '$(DLLEXT)'
	@echo LLAMA_DIR: $(LLAMA_DIR)  ZSTD: $(ZSTDLIB)
