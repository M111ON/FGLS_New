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
INCS   += -II:/llama.cpp/include -II:/llama.cpp/ggml/include
INCS   += -II:/llama.cpp/src
CFLAGS  = -O2 -std=c11 $(INCS)
CXXFLAGS = -O2 -std=c++17 $(INCS)
LDLIBS  = -lm
ZSTDLIB = ./zstd.dll
DLLDIR  = I:/llama/llama-b9733-bin-win-vulkan-x64
DLLS    = $(DLLDIR)/llama.dll $(DLLDIR)/ggml.dll $(DLLDIR)/ggml-base.dll $(DLLDIR)/ggml-cpu-x64.dll $(DLLDIR)/ggml-vulkan.dll
RUNNER_DLLS = ./libllama.dll ./ggml.dll ./ggml-base.dll ./ggml-cpu-x64.dll ./ggml-vulkan.dll

.PHONY: all clean run

all: test_swap_all.exe llama_pogls_runner_sid_v2.exe

test_swap_all.exe: test_swap_all.c gguf_index.h
	$(CC) $(CFLAGS) -o $@ $< $(DLLS) $(LDLIBS) $(ZSTDLIB)

test_swap_debug.exe: test_swap_debug.c
	$(CC) $(CFLAGS) -o $@ $< $(DLLS) $(LDLIBS) $(ZSTDLIB)

llama_pogls_runner_sid.exe: llama_pogls_runner_sid.c gguf_index.h sid_loader.h sid_cache.h
	$(CC) $(CFLAGS) -o $@ $< $(DLLS) $(LDLIBS) $(ZSTDLIB)

llama_pogls_runner_sid_v2.exe: llama_pogls_runner_sid_v2.o geo_jump.o kv_tensor_access.o
	$(CC) -m64 -O2 -o $@ $^ -L. -llibllama -lggml -lggml-base -lggml-cpu-x64 -lggml-vulkan -lstdc++ -lm $(ZSTDLIB)

llama_pogls_runner_sid_v2.o: llama_pogls_runner_sid_v2.c gguf_index.h sid_loader.h sid_cache.h bond_discovery.h hex_grid.h th_grid.h goldberg_sid.h sid_delta_ring.h sid_timetravel.h geo_addr.h dramtile_store.h kv_page_store.h kv_sid_evict.h kv_remap.h kv_remap_rail.h ../collection/geo_dram_tile.h
	$(CC) $(CFLAGS) -c -o $@ $<

geo_jump.o: ../collection/geo_jump_module/src/geo_jump.c
	$(CC) $(CFLAGS) -c -o $@ $<

kv_tensor_access.o: kv_tensor_access.cpp kv_tensor_access.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test_phase_rail.exe: ../test/c/test_phase_rail.c ../include/phase_rail.h ../include/rail_ring.h ../include/rail_sync.h
	$(CC) $(CFLAGS) -I../include -o $@ $< -lm

test_kv_remapping.exe: test_kv_remapping.c
	$(CC) $(CFLAGS) -o $@ $< -lm

test_kv_remap.exe: test_kv_remap.c kv_remap.h kv_remap_rail.h
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f *.exe *.o tests/*.exe

run: all
	.\llama_pogls_runner_sid.exe --help
