CC      = gcc
CXX     = C:/mingw64/bin/g++.exe
INCS    = -I. -I../collection -I../collection/src -I../collection/core
INCS   += -I../collection/core/core -I../collection/geopixel
INCS   += -I../collection/geopixel/Metatron/core -I../collection/pogls_engine
INCS   += -I../collection/geo_jump_module/include
INCS   += -I../collection/geopixel/hbv_bundle/Diamond_shell_encoder
INCS   += -I../collection/geopixel/hbv_bundle/Diamond_decode_hamburger
INCS   += -I../collection/geopixel/hbv_bundle/core
INCS   += -I../collection/Hfolder
INCS   += -I../collection/rdh
INCS   += -II:/llama.cpp/include -II:/llama.cpp/ggml/include
INCS   += -II:/llama.cpp/src
INCS   += -Ipogls_core -Ipogls_gguf -Ipogls_geo -Ipogls_dram -Ipogls_bermuda -Ipogls_kv -Ipogls_geopixel
CFLAGS  = -O2 -std=c11 -fno-strict-aliasing $(INCS)
CXXFLAGS = -O2 -std=c++17 $(INCS)
LDLIBS  = -lm
ZSTDLIB = ./zstd.dll
DLLDIR  = I:/llama/llama-b9733-bin-win-vulkan-x64
DLLS    = $(DLLDIR)/llama.dll $(DLLDIR)/ggml.dll $(DLLDIR)/ggml-base.dll $(DLLDIR)/ggml-cpu-x64.dll $(DLLDIR)/ggml-vulkan.dll
RUNNER_DLLS = ./libllama.dll ./ggml.dll ./ggml-base.dll ./ggml-cpu-x64.dll ./ggml-vulkan.dll

INSTALL_DIR ?= I:/FGLS_new/tools

.PHONY: all core modules tools test test_modules install clean help

# ── Default: build everything ──
all: core modules tools runner

# ── POGLS Core Library ──
POGLS_CORE_OBJS = pogls_core/pogls_platform.o pogls_core/pogls_compress.o pogls_core/pogls_addr.o pogls_core/pogls_meta.o

core: pogls_core.lib

# ── POGLS Standalone Modules ──
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

# ── Module Tests ──
test_modules: core modules
	@echo "═══ POGLS Module Tests ═══"
	@$(CC) $(CFLAGS) -o _test_core.exe pogls_core/test_pogls_core.c $(POGLS_CORE_OBJS) $(ZSTDLIB) -lm && ./_test_core.exe && $(RM) _test_core.exe
	@$(CC) $(CFLAGS) -o _test_gguf.exe pogls_gguf/test_pogls_gguf.c pogls_gguf/pogls_gguf.o && ./_test_gguf.exe && $(RM) _test_gguf.exe
	@$(CC) $(CFLAGS) -o _test_geo.exe pogls_geo/test_pogls_geo.c pogls_geo/pogls_geo.o -lm && ./_test_geo.exe && $(RM) _test_geo.exe
	@$(CC) $(CFLAGS) -o _test_dram.exe pogls_dram/test_pogls_dram.c pogls_dram/pogls_dram.o pogls_core/pogls_platform.o && ./_test_dram.exe && $(RM) _test_dram.exe
	@$(CC) $(CFLAGS) -o _test_bermuda.exe pogls_bermuda/test_pogls_bermuda.c pogls_bermuda/pogls_bermuda.o pogls_core/pogls_platform.o && ./_test_bermuda.exe && $(RM) _test_bermuda.exe
	@$(CC) $(CFLAGS) -o _test_kv.exe pogls_kv/test_pogls_kv.c pogls_kv/pogls_kv.o && ./_test_kv.exe && $(RM) _test_kv.exe
	@$(CC) $(CFLAGS) -o _test_geopixel.exe pogls_geopixel/test_pogls_geopixel.c pogls_geopixel/pogls_geopixel.o -lm && ./_test_geopixel.exe && $(RM) _test_geopixel.exe
	@echo "═══ All module tests done ═══"

pogls_core.lib: $(POGLS_CORE_OBJS)
	ar rcs $@ $^

pogls_core/pogls_platform.o: pogls_core/pogls_platform.c pogls_core/pogls_platform.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_core/pogls_compress.o: pogls_core/pogls_compress.c pogls_core/pogls_compress.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -c -o $@ $<

pogls_core/pogls_addr.o: pogls_core/pogls_addr.c pogls_core/pogls_addr.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_core/pogls_meta.o: pogls_core/pogls_meta.c pogls_core/pogls_meta.h pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── POGLS CLI Tools ──
tools: core pogls_compress.exe pogls_decompress.exe pogls_inspect.exe pogls_roundtrip.exe \
       addr_resolve.exe gguf_dump.exe \
       dramtile_dump.exe dramtile_bench.exe dramtile_cli.exe kv_delta_bench.exe kv_delta_test.exe \
       pogls_verify.exe pogls_build.exe pogls_cat.exe pogls_diff.exe pogls_test.exe \
       test_pogls_loader.exe pogls_loader_demo.exe

# Phase 1: Core CLI
pogls_compress.exe: pogls_tools/pogls_compress.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) -lm

pogls_decompress.exe: pogls_tools/pogls_decompress.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) -lm

pogls_inspect.exe: pogls_tools/pogls_inspect.c pogls_core/pogls_core.h pogls_meta.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core -lm

pogls_roundtrip.exe: pogls_tools/pogls_roundtrip.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) -lm

# Phase 2: Address + GGUF
addr_resolve.exe: pogls_tools/addr_resolve.c pogls_core/pogls_addr.h gguf_reader.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core -lm

gguf_dump.exe: pogls_tools/gguf_dump.c gguf_reader.h
	$(CC) $(CFLAGS) -o $@ $< -lm

# Phase 3: Store + Delta
dramtile_dump.exe: pogls_tools/dramtile_dump.c dramtile_store.h
	$(CC) $(CFLAGS) -o $@ $< -lm

dramtile_bench.exe: pogls_tools/dramtile_bench.c dramtile_store.h
	$(CC) $(CFLAGS) -o $@ $< -lm

dramtile_cli.exe: pogls_tools/dramtile_cli.c dramtile_store.h pogls_core/pogls_platform.h ../collection/geo_dram_tile.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core -lm

kv_delta_bench.exe: pogls_tools/kv_delta_bench.c kv_remap.h kv_remap_diamond.h
	$(CC) $(CFLAGS) -o $@ $< -lm

kv_delta_test.exe: pogls_tools/kv_delta_test.c kv_remap.h kv_remap_diamond.h
	$(CC) $(CFLAGS) -o $@ $< -lm

# Phase 4: Pipeline
pogls_verify.exe: pogls_tools/pogls_verify.c pogls_core/pogls_core.h pogls_meta.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core -lm

pogls_build.exe: pogls_tools/pogls_build.c pogls_core/pogls_core.h pogls_meta.h gguf_reader.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core -lm

pogls_cat.exe: pogls_tools/pogls_cat.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core -lm

pogls_diff.exe: pogls_tools/pogls_diff.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -o $@ $< -L. -lpogls_core -lm

pogls_test.exe: pogls_tools/pogls_test.c pogls_core/pogls_core.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< -L. -lpogls_core $(ZSTDLIB) -lm

# ── POGLS Loader (standalone) ──
test_pogls_loader.exe: pogls_tools/test_pogls_loader.c pogls_core/pogls_loader.h
	$(CC) $(CFLAGS) -DPOGLS_LOADER_IMPLEMENTATION -o $@ $< -lm

pogls_loader_demo.exe: pogls_tools/pogls_loader_demo.c pogls_core/pogls_loader.h
	$(CC) $(CFLAGS) -DPOGLS_LOADER_IMPLEMENTATION -o $@ $< -lm

libpogls_loader.a: pogls_core/pogls_loader.o
	ar rcs $@ $^

pogls_core/pogls_loader.o: pogls_core/pogls_loader.c pogls_core/pogls_loader.h
	$(CC) $(CFLAGS) -DPOGLS_LOADER_IMPLEMENTATION -c -o $@ $<

# ── Runner (existing) ──
runner: llama_pogls_runner_sid_v2.exe gguf_to_pogls.exe

llama_pogls_runner_sid_v2.exe: llama_pogls_runner_sid_v2.o geo_jump.o kv_tensor_access.o
	$(CC) -m64 -O2 -Wl,--stack,16777216 -o $@ $^ -L. -llibllama -lggml -lggml-base -lggml-cpu-x64 -lggml-vulkan -lstdc++ -lm $(ZSTDLIB)

llama_pogls_runner_sid_v2.o: llama_pogls_runner_sid_v2.c
	$(CC) $(CFLAGS) -c -o $@ $<

gguf_to_pogls.exe: gguf_to_pogls.c pogls_meta.h gguf_index.h addr_space.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -o $@ $< $(ZSTDLIB) -lm

geo_jump.o: ../collection/geo_jump_module/src/geo_jump.c
	$(CC) $(CFLAGS) -c -o $@ $<

kv_tensor_access.o: kv_tensor_access.cpp kv_tensor_access.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test_core: core
	$(CC) $(CFLAGS) -o test_pogls_core.exe pogls_core/test_pogls_core.c $(POGLS_CORE_OBJS) $(ZSTDLIB) -lm
	./test_pogls_core.exe

# ── Tests ──
test: tools test_modules
	@echo "═══ Running toolchain tests ═══"
	@./pogls_test.exe && echo "[1/7] core tests: PASS" || echo "[1/7] core tests: FAIL"
	@./pogls_roundtrip.exe pogls_core/pogls_platform.h && echo "[2/7] roundtrip: PASS" || echo "[2/7] roundtrip: FAIL"
	@./pogls_verify.exe I:/FGLS_new/test_verify.pogls && echo "[3/7] verify v1: PASS" || echo "[3/7] verify v1: FAIL (expected for v1)"
	@./pogls_cat.exe _test_cat.bin pogls_core/pogls_platform.h pogls_core/pogls_compress.h && echo "[4/7] cat: PASS" || echo "[4/7] cat: FAIL"
	@./pogls_diff.exe pogls_core/pogls_platform.h pogls_core/pogls_platform.h && echo "[5/7] diff identical: PASS" || echo "[5/7] diff identical: FAIL"
	@./addr_resolve.exe --name "blk.0.attn_q.weight" >nul 2>&1 && echo "[6/7] addr_resolve: PASS" || echo "[6/7] addr_resolve: FAIL"
	@./kv_delta_test.exe 2>&1 | grep "ALL PASS" >nul && echo "[7/7] kv_delta_test: PASS" || echo "[7/7] kv_delta_test: FAIL"
	@echo "═══ Done ═══"
	@rm -f _test_cat.bin 2>nul

# ── Install ──
install: tools
	@echo "Installing tools to $(INSTALL_DIR)..."
	@test -d "$(INSTALL_DIR)" || mkdir -p "$(INSTALL_DIR)"
	@cp pogls_compress.exe "$(INSTALL_DIR)/"
	@cp pogls_decompress.exe "$(INSTALL_DIR)/"
	@cp pogls_inspect.exe "$(INSTALL_DIR)/"
	@cp pogls_roundtrip.exe "$(INSTALL_DIR)/"
	@cp addr_resolve.exe "$(INSTALL_DIR)/"
	@cp gguf_dump.exe "$(INSTALL_DIR)/"
	@cp dramtile_dump.exe "$(INSTALL_DIR)/"
	@cp dramtile_bench.exe "$(INSTALL_DIR)/"
	@cp kv_delta_bench.exe "$(INSTALL_DIR)/"
	@cp kv_delta_test.exe "$(INSTALL_DIR)/"
	@cp pogls_verify.exe "$(INSTALL_DIR)/"
	@cp pogls_build.exe "$(INSTALL_DIR)/"
	@cp pogls_cat.exe "$(INSTALL_DIR)/"
	@cp pogls_diff.exe "$(INSTALL_DIR)/"
	@cp pogls_core.lib "$(INSTALL_DIR)/"
	@echo "Installed 15 tools + pogls_core.lib to $(INSTALL_DIR)"

# ── Clean ──
clean:
	rm -f *.exe *.o tests/*.exe pogls_core/*.o pogls_core.lib pogls_tools/*.o \
	      pogls_gguf/*.o pogls_geo/*.o pogls_dram/*.o \
	      pogls_bermuda/*.o pogls_kv/*.o pogls_geopixel/*.o

# ── Help ──
help:
	@echo POGLS Toolchain — Build Targets
	@echo ────────────────────────────────
	@echo   all        Build core + tools + runner
	@echo   core       Build pogls_core.lib (static)
	@echo   tools      Build all 15 CLI tools
	@echo   runner     Build llama_pogls_runner + gguf_to_pogls
	@echo   test       Run toolchain test suite
	@echo   install    Install tools to $(INSTALL_DIR)
	@echo   clean      Remove all build artifacts
	@echo   help       Show this help
